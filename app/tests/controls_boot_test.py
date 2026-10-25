"""Windows controls acceptance in real processes, with isolated config/data.

Usage: python -I app/tests/controls_boot_test.py <product> <game root/ISO> <new output> [d3d12|vulkan|both]
Posts input only to the test process. Never changes the supplied product/game.
"""
import copy
import ctypes
from ctypes import wintypes as w
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import tomllib

spec = importlib.util.spec_from_file_location('setup_test', Path(__file__).with_name('setup_boot_test.py'))
win = importlib.util.module_from_spec(spec)
spec.loader.exec_module(win)
focus_spec = importlib.util.spec_from_file_location('close_test', Path(__file__).with_name('close_boot_test.py'))
focus = importlib.util.module_from_spec(focus_spec)
focus_spec.loader.exec_module(focus)


def main():
    source, game, output = (Path(value).resolve() for value in sys.argv[1:4])
    mode = sys.argv[4] if len(sys.argv) > 4 else 'both'
    output.mkdir(parents=True, exist_ok=False)
    results = []
    apis = ('d3d12', 'vulkan') if mode == 'both' else (mode,)
    for api in apis:
        product = Path(tempfile.mkdtemp(prefix='narutorise-controls-'))
        for item in source.iterdir():
            if item.name == 'narutorise.exe' or item.suffix.lower() == '.dll':
                shutil.copy2(item, product / item.name)
        config_path = product / 'narutorise.toml'
        basic = dict(renderer='native', native_backend=api, fullscreen=False,
                     skip_intro_videos=True, frame_rate=60, input_backend='sdl',
                     mnk_mode=True, mnk_mouse=True, custom_game_root=str(game),
                     launcher_language='pt_BR', check_for_updates=False,
                     preserved_setting='keep', show_fps_overlay=False)
        text = '\n'.join(f'{key} = {json.dumps(value, ensure_ascii=False)}' for key, value in basic.items())
        text += '\n[controls]\nversion = 1\n'
        for port in range(1, 5):
            text += f'\n[controls.player{port}]\n'
            text += 'device = "keyboard"\ndevice_name = "Keyboard and mouse"\n' if port == 1 else 'device = ""\n'
            text += f'sensitivity = {1.75 if port == 1 else 1 + port / 3}\n'
            text += 'left_stick = 0.25\nright_stick = 0.27\nrumble_low = 0.4\nrumble_high = 0.7\n'
            text += f'\n[controls.player{port}.pad]\nkeybind_a = "button:32768"\n'
            text += f'\n[controls.player{port}.keys]\nkeybind_b = "{chr(65 + port)}"\n'
        config_path.write_text(text, encoding='utf-8')

        def config():
            return tomllib.loads(config_path.read_text(encoding='utf-8'))

        def run(restarted):
            before_logs = set((product / 'logs').glob('*.log'))
            process = subprocess.Popen([str(product / 'narutorise.exe'), '--setup'], cwd=product,
                                       env=dict(os.environ, REX_USER_DATA_ROOT=str(product / 'user')))
            try:
                hwnd = win.wait_for(lambda: win.window(process.pid), process)

                def log():
                    files = set((product / 'logs').glob('*.log')) - before_logs
                    return max(files, key=lambda p: p.stat().st_mtime).read_text(encoding='utf-8', errors='replace') if files else ''

                win.wait_for(lambda: 'Setup screen:' in log(), process, timeout=45)

                def key(value):
                    scan = win.user32.MapVirtualKeyW(value, 0)
                    flags = 1 | (scan << 16) | ((1 << 24) if 0x21 <= value <= 0x28 or value in (0x2d, 0x2e) else 0)
                    win.user32.PostMessageW(hwnd, 0x100, value, flags)
                    win.user32.PostMessageW(hwnd, 0x101, value, flags | 0xc0000000)
                    time.sleep(.12)

                def resize(width, height):
                    rect = w.RECT(0, 0, width, height)
                    style = win.user32.GetWindowLongW(hwnd, -16)
                    extended = win.user32.GetWindowLongW(hwnd, -20)
                    assert win.user32.AdjustWindowRectEx(ctypes.byref(rect), style, False, extended)
                    assert win.user32.SetWindowPos(hwnd, 0, 0, 0, rect.right - rect.left, rect.bottom - rect.top, 0x14)
                    time.sleep(.3)
                    actual = w.RECT()
                    win.user32.GetClientRect(hwnd, ctypes.byref(actual))
                    assert (actual.right, actual.bottom) == (width, height)

                def click(dx, dy):
                    rect = w.RECT()
                    win.user32.GetClientRect(hwnd, ctypes.byref(rect))
                    ratio = max(1, rect.bottom / 720)
                    width = min(1040, rect.right / ratio - 48)
                    height = min(586.667, rect.bottom / ratio - 32)
                    x = round((rect.right / ratio - width) / 2 * ratio + dx * ratio)
                    y = round((rect.bottom / ratio - height) / 2 * ratio + dy * ratio)
                    xy = (y << 16) | x
                    win.user32.PostMessageW(hwnd, 0x200, 0, xy)
                    win.user32.PostMessageW(hwnd, 0x201, 1, xy)
                    win.user32.PostMessageW(hwnd, 0x202, 0, xy)
                    time.sleep(.18)

                def shot(name):
                    time.sleep(.15)
                    win.capture(hwnd, output / f'{api}-{name}.png')

                def open_controls():
                    key(0x28)
                    key(13)
                    for _ in range(3):
                        key(0x22)
                    key(13)
                    key(0x28)  # Devices -> Buttons, through the keyboard focus path.
                    key(13)  # Enter the category's fields.

                def begin_capture():
                    focus.focus_window(hwnd)
                    click(926, 183)  # Primary A binding; the secondary slot stays separate.

                resize(1280, 720)
                focus.focus_window(hwnd)
                time.sleep(.3)
                open_controls()
                shot('after-restart' if restarted else 'initial')
                if not restarted:
                    unchanged = config_path.read_bytes()
                    click(150, 113)
                    shot('devices')
                    key(0x22)
                    shot('player-two-none')
                    key(0x21)
                    click(150, 237)
                    shot('keyboard-triggers')
                    click(150, 281)
                    shot('movement-camera')
                    click(150, 194)
                    click(500, 500)
                    shot('keyboard-directions')
                    click(150, 194)
                    assert config_path.read_bytes() == unchanged, 'view navigation changed configuration'
                    begin_capture()
                    shot('capture-inline')
                    key(0x47)
                    win.wait_for(lambda: config()['controls']['player1']['keys']['keybind_a'] == 'G', process)
                    shot('remapped')
                    key(27)  # Fields -> selected category.
                    key(27)  # Back to Settings; open the dedicated configuration again.
                    key(13)
                    click(150, 194)
                    shot('reopened')
                    assert config()['controls']['player1']['keys']['keybind_a'] == 'G'
                else:
                    assert config()['controls']['player1']['keys']['keybind_a'] == 'G'
                    saved = config_path.read_bytes()
                    begin_capture()
                    shot('cancel-start')
                    key(27)
                    shot('canceled')
                    assert config_path.read_bytes() == saved, 'canceled capture wrote configuration'
                    begin_capture()
                    shot('delete-start')
                    key(0x2e)
                    shot('delete-sent')
                    win.wait_for(lambda: config()['controls']['player1']['keys']['keybind_a'] == '', process, timeout=5)
                    others = copy.deepcopy(config()['controls'])
                    before_reset = config_path.read_bytes()
                    click(150, 451)  # Restore defaults, with Cancel focused first.
                    key(13)
                    assert config_path.read_bytes() == before_reset, 'canceled restore wrote configuration'
                    click(150, 451)
                    shot('restore-confirmation')
                    key(9)
                    key(13)
                    win.wait_for(lambda: config()['controls']['player1']['keys']['keybind_a'] == 'Space', process)
                    restored = config()
                    port = restored['controls']['player1']
                    assert port['device'] == 'keyboard'
                    assert port['pad']['keybind_a'] == 'button:4096'
                    assert port['sensitivity'] == 1 and port['left_stick'] == .15
                    assert port['rumble_low'] == 1 and port['rumble_high'] == 1
                    assert all(restored['controls'][f'player{p}'] == others[f'player{p}'] for p in (2, 3, 4))
                    assert restored['preserved_setting'] == 'keep'
                    shot('restored-720p')
                    resize(1920, 1080)
                    shot('restored-1080p')
                    resize(2560, 1080)
                    shot('restored-wide')
                win.user32.PostMessageW(hwnd, 0x10, 0, 0)
                process.wait(timeout=25)
                assert process.returncode == 0, process.returncode
                shutil.copy2(config_path, output / f'{api}-{"restarted" if restarted else "first"}.toml')
                files = set((product / 'logs').glob('*.log')) - before_logs
                if files:
                    shutil.copy2(max(files, key=lambda p: p.stat().st_mtime), output / f'{api}-{"restarted" if restarted else "first"}.log')
                result = dict(api=api, restarted=restarted, passed=True, exit=process.returncode, product=str(product))
                results.append(result)
                (output / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
                print(json.dumps(result), flush=True)
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

        run(False)
        run(True)
    print('PASS: controls autosave/reopen/restart/cancel/delete/restore/isolation and v4 screenshots', flush=True)


if __name__ == '__main__':
    main()
