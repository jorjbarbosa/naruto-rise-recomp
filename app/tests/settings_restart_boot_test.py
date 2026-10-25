"""API/GPU edits save first, defer by default, and restart only on confirmation.

Usage: python -I app/tests/settings_restart_boot_test.py <product> <game.iso> <new output>
Uses isolated product/configuration/saves and closes only its own test processes.
"""
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import tomllib


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


win = load('setup_test', 'setup_boot_test.py')
focus = load('close_test', 'close_boot_test.py')
kernel = ctypes.windll.kernel32
kernel.OpenProcess.argtypes = [win.w.DWORD, win.w.BOOL, win.w.DWORD]
kernel.OpenProcess.restype = win.w.HANDLE
kernel.GetExitCodeProcess.argtypes = [win.w.HANDLE, ctypes.POINTER(win.w.DWORD)]
kernel.CloseHandle.argtypes = [win.w.HANDLE]


class RestartedProcess:
    def __init__(self, pid):
        self.pid = pid
        self.handle = kernel.OpenProcess(0x100000 | 0x1000, False, pid)
        assert self.handle, f'cannot monitor replacement process {pid}'

    def poll(self):
        code = win.w.DWORD()
        assert kernel.GetExitCodeProcess(self.handle, ctypes.byref(code))
        return None if code.value == 259 else code.value

    def wait(self, timeout=25):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            code = self.poll()
            if code is not None:
                return code
            time.sleep(.05)
        raise subprocess.TimeoutExpired(str(self.pid), timeout)


def main():
    source, iso, output = (Path(arg).resolve() for arg in sys.argv[1:])
    output.mkdir(parents=True, exist_ok=False)
    results = []
    for api in ('d3d12', 'vulkan'):
        for stage in ('setup', 'game'):
            name = api + '-' + stage
            product = Path(tempfile.mkdtemp(prefix='narutorise-restart-'))
            for item in source.iterdir():
                if item.name == 'narutorise.exe' or item.suffix.lower() == '.dll':
                    shutil.copy2(item, product / item.name)
            user = product / 'saves-ação'
            config_path = product / 'narutorise.toml'
            config = dict(renderer='native', native_backend=api, graphics_device='', fullscreen=False,
                          skip_intro_videos=True, custom_game_root=str(iso), launcher_language='pt_BR',
                          check_for_updates=False, dlc_source_path='')
            config_path.write_text('\n'.join(f'{k} = {json.dumps(v)}' for k, v in config.items()), encoding='utf-8')
            args = ['--setup'] if stage == 'setup' else []
            with (output / (name + '-console.log')).open('wb') as console:
                original = subprocess.Popen([str(product / 'narutorise.exe'), *args], cwd=product,
                                            env=dict(os.environ, REX_USER_DATA_ROOT=str(user)),
                                            stdout=console, stderr=subprocess.STDOUT)
                processes = [original]

                def latest_log():
                    files = list((product / 'logs').glob('*.log'))
                    return max(files, key=lambda p: p.stat().st_mtime) if files else None

                def text():
                    log = latest_log()
                    return log.read_text(encoding='utf-8', errors='replace') if log else ''

                def saved():
                    return tomllib.loads(config_path.read_text(encoding='utf-8'))

                try:
                    hwnd = win.wait_for(lambda: win.window(original.pid), original)
                    ready = 'Setup screen: requested' if stage == 'setup' else 'Native renderer backend:'
                    win.wait_for(lambda: ready in text(), original)
                    focus.focus_window(hwnd)
                    time.sleep(1 if stage == 'setup' else 5)

                    def key(value):
                        if win.user32.GetForegroundWindow() != hwnd:
                            focus.focus_window(hwnd)
                        flags = 1 | (win.user32.MapVirtualKeyW(value, 0) << 16)
                        if 0x21 <= value <= 0x28:
                            flags |= 1 << 24
                        win.user32.PostMessageW(hwnd, 0x100, value, flags)
                        win.user32.PostMessageW(hwnd, 0x101, value, flags | 0xc0000000)
                        time.sleep(.2)

                    if stage == 'setup':
                        key(0x28)
                    else:
                        key(0x70)
                    key(13)  # Settings.
                    key(0x22)  # Display, focused on graphics API.
                    other_api = 'vulkan' if api == 'd3d12' else 'd3d12'
                    key(0x27)
                    win.wait_for(lambda: saved()['native_backend'] == other_api, original)
                    time.sleep(.6)
                    assert original.poll() is None, 'API edit restarted before confirmation'
                    win.capture(hwnd, output / (name + '-api-prompt.png'))
                    key(13)  # Default action: Later.
                    assert original.poll() is None and saved()['native_backend'] == other_api
                    key(0x28)
                    key(0x27)  # Automatic -> actual GPU, with another prompt.
                    win.wait_for(lambda: bool(saved()['graphics_device']), original)
                    win.capture(hwnd, output / (name + '-gpu-prompt.png'))
                    key(27)  # Esc also keeps the saved option for later.
                    assert original.poll() is None and saved()['graphics_device']
                    key(0x25)  # GPU -> Automatic; explicitly confirm this prompt.
                    win.wait_for(lambda: saved()['graphics_device'] == '', original)
                    first_log = latest_log()
                    key(9)
                    key(13)  # Tab from Later to Restart now.
                    original.wait(timeout=25)
                    assert original.returncode == 0, original.returncode
                    old_text = first_log.read_text(encoding='utf-8', errors='replace')
                    match = re.search(r'Restarting for saved settings: process (\d+)', old_text)
                    assert match, 'restart was not explicitly launched'
                    child = RestartedProcess(int(match[1]))
                    processes.append(child)
                    child_hwnd = win.wait_for(lambda: win.window(child.pid), child)
                    win.wait_for(lambda: f'Renderer: native; backend: {other_api}' in text() and
                                 'Setup screen: requested' in text(), child)
                    assert (user / 'launch.marker').exists(), 'replacement lost the user-data root'
                    assert saved()['custom_game_root'] == str(iso)
                    win.capture(child_hwnd, output / (name + '-restarted.png'))
                    win.user32.PostMessageW(child_hwnd, 0x10, 0, 0)
                    assert child.wait() == 0
                    assert not (user / 'launch.marker').exists()
                    for log in (product / 'logs').glob('*.log'):
                        shutil.copy2(log, output / (name + '-' + log.name))
                    results.append(dict(case=name, passed=True, replacement_api=other_api))
                    print(json.dumps(results[-1]), flush=True)
                    (output / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
                finally:
                    for process in reversed(processes):
                        if process.poll() is None:
                            hwnd = win.window(process.pid)
                            if hwnd:
                                win.user32.PostMessageW(hwnd, 0x10, 0, 0)
                            process.wait(timeout=25)
                        if isinstance(process, RestartedProcess):
                            kernel.CloseHandle(process.handle)
    print('PASS: saved API/GPU edits, Later/Enter/Esc, explicit restart and preserved paths on both APIs', flush=True)


if __name__ == '__main__':
    main()
