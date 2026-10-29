"""Verify phase evidence through first presentation in the actual native renderers."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time


def close_window(pid):
    import ctypes
    from ctypes import wintypes
    user = ctypes.windll.user32

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def close(hwnd, _):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid:
            user.PostMessageW(hwnd, 0x10, 0, 0)
        return True
    user.EnumWindows(close, 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('product', type=Path)
    parser.add_argument('game', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('This driver uses Windows WM_CLOSE; validate Linux presentation/close manually')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    allowed = {'SYSTEMROOT', 'WINDIR', 'PATH', 'COMSPEC', 'PATHEXT', 'OS',
               'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE', 'DISPLAY',
               'WAYLAND_DISPLAY', 'XDG_RUNTIME_DIR'}
    env = {key: value for key, value in os.environ.items() if key.upper() in allowed}
    executable = args.product.resolve() / ('narutorise.exe' if os.name == 'nt' else 'narutorise')
    results = []
    for backend in ('d3d12', 'vulkan') if os.name == 'nt' else ('vulkan',):
        folder = output / backend
        folder.mkdir()
        temporary = folder / 'temp'
        temporary.mkdir()
        data = folder / 'data'
        process = subprocess.Popen([str(executable), '--data_root=' + str(data),
                                  '--cache_root=' + str(folder / 'cache'),
                                  '--game_data_root=' + str(args.game.resolve()),
                                  '--renderer=native', '--native_backend=' + backend,
                                  '--show_startup_menu=false', '--setup=false',
                                  '--check_for_updates=false', '--crash_reporting=false',
                                  '--skip_intro_videos=true'], cwd=folder,
                                 env={**env, 'TEMP': str(temporary), 'TMP': str(temporary)},
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        deadline = time.monotonic() + 60
        manifest = None
        try:
            while time.monotonic() < deadline:
                paths = list((data / 'incidents').glob('session-*/manifest.json'))
                if paths:
                    manifest = json.loads(paths[0].read_text(encoding='utf-8'))
                    history = (paths[0].parent / 'events.jsonl').read_text(encoding='utf-8') if (paths[0].parent / 'events.jsonl').exists() else ''
                    parsed = []
                    for line in history.splitlines():
                        try:
                            parsed.append(json.loads(line))
                        except json.JSONDecodeError:
                            pass
                    if any(e.get('operation') == 'first-game-present' and e.get('state') == 'end' for e in parsed):
                        close_window(manifest['child_pid'])
                        break
                if process.poll() is not None:
                    break
                time.sleep(0.1)
            stdout, stderr = process.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            # Keep the observer alive to record the test child's actual exit.
            if manifest and manifest.get('child_pid'):
                import ctypes
                handle = ctypes.windll.kernel32.OpenProcess(1, False, manifest['child_pid'])
                if handle:
                    ctypes.windll.kernel32.TerminateProcess(handle, 124)
                    ctypes.windll.kernel32.CloseHandle(handle)
            stdout, stderr = process.communicate(timeout=10)
            (folder / 'terminal.log').write_bytes(stdout + stderr)
            raise
        (folder / 'terminal.log').write_bytes(stdout + stderr)
        manifests = list((data / 'incidents').glob('session-*/manifest.json'))
        assert len(manifests) == 1, (backend, process.returncode)
        manifest = json.loads(manifests[0].read_text(encoding='utf-8'))
        assert manifest['classification'] == 'normal-exit', (backend, manifest)
        assert process.returncode == 0, (backend, manifest)
        events = [json.loads(line) for line in (manifests[0].parent / 'events.jsonl').read_text(encoding='utf-8').splitlines()]
        completed = {event['operation'] for event in events if event.get('state') == 'end'}
        assert {'device-create', 'swapchain-attach', 'first-frame-submit', 'first-native-present', 'first-game-present'} <= completed, (backend, completed)
        assert any(e.get('operation') == 'pipeline-precompile' for e in events), (backend, events)
        assert manifest['metadata']['backend_active'] == backend, manifest
        results.append({'backend': backend, 'exit_code': 0, 'manifest': str(manifests[0]), 'completed_phases': sorted(completed)})
        print('PASS:', backend, 'first game presentation, phase evidence and normal close', flush=True)
    (output / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
