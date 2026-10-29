"""Verify Play uses complete game data rather than code-generation inputs.

Usage: python -I app/tests/prelaunch_data_boot_test.py <product> <extracted game> <game.iso> <new output>
Windows only. Uses isolated configuration/saves, a partial XEX fixture and a
junction to the supplied complete game folder. Never changes the source data.
"""
import _winapi
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


win = load('setup_test', 'setup_boot_test.py')
focus = load('close_test', 'close_boot_test.py')


def main():
    source, game, iso, output = (Path(value).resolve() for value in sys.argv[1:])
    output.mkdir(parents=True, exist_ok=False)
    product = Path(tempfile.mkdtemp(prefix='narutorise-prelaunch-data-'))
    for item in source.iterdir():
        if item.name == 'narutorise.exe' or item.suffix.lower() == '.dll':
            shutil.copy2(item, product / item.name)
    partial = product / 'game_root'
    partial.mkdir()
    shutil.copy2(game / 'default.xex', partial / 'default.xex')
    _winapi.CreateJunction(str(game), str(product / 'game'))
    results = []

    for api in ('d3d12', 'vulkan'):
        for kind, root in (('incomplete', partial), ('automatic', None), ('folder', game), ('iso', iso)):
            name = kind + '-' + api
            user = product / ('user-' + name)
            config = dict(renderer='native', native_backend=api, fullscreen=False, skip_intro_videos=True,
                          check_for_updates=False, input_backend='sdl', launcher_language='pt_BR',
                          dlc_source_path=str(source / 'dlc'))
            if root:
                config['custom_game_root'] = str(root)
            (product / 'data').mkdir(exist_ok=True)
            (product / 'data/narutorise.toml').write_text(
                '\n'.join(f'{key} = {json.dumps(value)}' for key, value in config.items()), encoding='utf-8')
            before = set((product / 'data/logs').glob('*.log'))
            with (output / (name + '-console.log')).open('wb') as console:
                process = subprocess.Popen([str(product / 'narutorise.exe'), '--setup'], cwd=product,
                                           env=dict(os.environ, REX_DATA_ROOT=str(product / "data"), REX_USER_DATA_ROOT=str(user)),
                                           stdout=console, stderr=subprocess.STDOUT)
                log = None

                def text():
                    nonlocal log
                    logs = set((product / 'data/logs').glob('*.log')) - before
                    if logs:
                        log = max(logs, key=lambda path: path.stat().st_mtime)
                    return log.read_text(encoding='utf-8', errors='replace') if log else ''

                try:
                    hwnd = win.wait_for(lambda: win.window(process.pid), process)
                    reason = 'game-not-ready' if kind == 'incomplete' else 'requested'
                    win.wait_for(lambda: f'Setup screen: {reason}' in text(), process)
                    focus.focus_window(hwnd)
                    time.sleep(.5)
                    if kind == 'incomplete':
                        assert ': incomplete' in text()
                        assert 'Guest memory arena mapped' not in text()
                        win.capture(hwnd, output / (name + '.png'))
                    else:
                        if kind == 'automatic':
                            assert 'Game data: ' + str(game) + ' (folder)' in text()
                        flags = 1 | (win.user32.MapVirtualKeyW(13, 0) << 16)
                        win.user32.PostMessageW(hwnd, 0x100, 13, flags)
                        win.user32.PostMessageW(hwnd, 0x101, 13, flags | 0xc0000000)
                        marker = user / 'launch.marker'
                        win.wait_for(lambda: not marker.exists() and 'Native renderer backend:' in text(), process)
                        # The first Swap precedes loading the archives; survival
                        # after it is essential to catch the reported crash.
                        until = time.monotonic() + 8
                        while time.monotonic() < until:
                            assert process.poll() is None, f'{name}: exited {process.returncode}'
                            time.sleep(.1)
                        assert 'access violation' not in text() and '[FATAL]' not in text()
                        win.capture(hwnd, output / (name + '.png'))
                    win.user32.PostMessageW(hwnd, 0x10, 0, 0)
                    process.wait(timeout=25)
                    assert process.returncode == 0, f'{name}: close exited {process.returncode}'
                    results.append(dict(case=name, passed=True, exit=process.returncode))
                    print(json.dumps(results[-1]), flush=True)
                    (output / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
                finally:
                    if process.poll() is None:
                        hwnd = win.window(process.pid)
                        if hwnd:
                            win.user32.PostMessageW(hwnd, 0x10, 0, 0)
                        try:
                            process.wait(timeout=25)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
                    text()
                    if log:
                        shutil.copy2(log, output / (name + '.log'))
    print('PASS: incomplete data blocked; automatic, folder and ISO Play boots on D3D12 and Vulkan', flush=True)


if __name__ == '__main__':
    main()
