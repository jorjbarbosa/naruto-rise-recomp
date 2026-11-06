"""Capture the in-game settings drawn as the game's Options screen, in the game.

Usage: python -I app/tests/game_style_boot_test.py <product folder> <game folder or .iso> <new output folder>
For each API: the game's own Options screen opening the settings, with a
change it keeps; the F1 menu keeping the menus' own panel; then the opening,
every tab, the controls page, the end of a long list, a tab switch and the
closing at 1280x720, the page at 1920x1080 and 2560x1080 (21:9) and the opening
at 21:9.
Uses isolated configuration and saves. Does not change the supplied product.
Keys are real input (SendInput) to the focused test window: keep the desktop free.
"""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import ctypes
from ctypes import wintypes as w

spec = importlib.util.spec_from_file_location('setup_test', Path(__file__).with_name('setup_boot_test.py'))
win = importlib.util.module_from_spec(spec)
spec.loader.exec_module(win)
spec = importlib.util.spec_from_file_location('close_test', Path(__file__).with_name('close_boot_test.py'))
real = importlib.util.module_from_spec(spec)
spec.loader.exec_module(real)

F1, ENTER, ESCAPE, PAGE_DOWN, LEFT, DOWN = 0x70, 0x0D, 0x1B, 0x22, 0x25, 0x28
PAD_A, PAD_DOWN = 0x20, 0x32  # The game's A and d-pad down with the keyboard (mnk_mode).
SETTLED = 1.5  # Longer than the opening (about 0.8 s) and a tab switch.
GAME_KEY = .08  # How long a key of the game's controller stays down.


def main():
    source, game, output = (Path(arg).resolve() for arg in sys.argv[1:])
    output.mkdir(parents=True, exist_ok=False)
    for api in ('d3d12', 'vulkan'):
        product = Path(tempfile.mkdtemp(prefix='narutorise-game-style-'))
        for item in source.iterdir():
            if item.name == 'narutorise.exe' or item.suffix.lower() == '.dll':
                shutil.copy2(item, product/item.name)
        config = dict(renderer='native', native_backend=api, fullscreen=False, skip_intro_videos=True,
                      show_startup_menu=False, frame_rate=60, mnk_mode=True, mnk_mouse=False,
                      custom_game_root=str(game), launcher_language='en', check_for_updates=False,
                      show_fps_overlay=False, crash_reporting=False, crash_reporting_asked=True)
        (product/'narutorise.toml').write_text('\n'.join(f'{key} = {json.dumps(value)}' for key, value in config.items()),
                                               encoding='utf-8')
        process = subprocess.Popen([str(product/'narutorise.exe')], cwd=product,
                                   env=dict(os.environ, REX_USER_DATA_ROOT=str(product/'user')))

        def log():
            files = list((product/'data/logs').glob('*.log'))
            return max(files, key=lambda p: p.stat().st_mtime).read_text(encoding='utf-8', errors='replace') if files else ''
        try:
            hwnd = win.wait_for(lambda: win.window(process.pid), process, timeout=60)
            win.wait_for(lambda: 'Native renderer backend:' in log() and not (product/'data/launch.marker').exists(),
                         process, timeout=90)

            def key(value, pause=.15, held=0.0):
                # The keyboard reaches the game through real input only; the
                # game's controller reads the keys once a frame, so its keys
                # stay down for a while.
                if win.user32.GetForegroundWindow() != hwnd:
                    real.focus_window(hwnd)
                extended = 1 if 0x21 <= value <= 0x28 else 0
                for up in (False, True):
                    event = real.Input(type=1)
                    event.key = real.Keyboard(value, win.user32.MapVirtualKeyW(value, 0), extended | (2 if up else 0), 0, 0)
                    assert win.user32.SendInput(1, ctypes.byref(event), ctypes.sizeof(real.Input)) == 1
                    if not up:
                        time.sleep(held)
                time.sleep(pause)

            def shot(name, delay=0.0):
                time.sleep(delay)
                win.capture(hwnd, output/f'{api}-{name}.png')

            def resize(width, height):
                rect = w.RECT(0, 0, width, height)
                style = win.user32.GetWindowLongW(hwnd, -16)
                extended = win.user32.GetWindowLongW(hwnd, -20)
                assert win.user32.AdjustWindowRectEx(ctypes.byref(rect), style, False, extended)
                assert win.user32.SetWindowPos(hwnd, 0, 0, 0, rect.right - rect.left, rect.bottom - rect.top, 0x14)
                time.sleep(.5)
                actual = w.RECT()
                win.user32.GetClientRect(hwnd, ctypes.byref(actual))
                assert (actual.right, actual.bottom) == (width, height), (actual.right, actual.bottom)

            resize(1280, 720)
            time.sleep(4)  # Past the shortcut hint.
            # The game's own Options screen opens the settings with the game's
            # options first; a change made there stays in the game's options.
            opened = "The game's Options screen opened the settings"

            def game_options(count, prefix=None):
                for _ in range(3):
                    key(PAD_DOWN, .6, GAME_KEY)  # OPTIONS, from STORY MODE.
                key(PAD_A, 0, GAME_KEY)
                win.wait_for(lambda: log().count(opened) == count, process, timeout=10)
                if prefix:
                    shot(prefix + '-open-a', .1)
                    shot(prefix + '-open-b', .1)
                    shot(prefix + '-open-c', .15)
                    shot(prefix + '-open', SETTLED)
                else:
                    shot(f'options-{count}', SETTLED + 1)

            def leave_options(count):
                key(ESCAPE, 0)
                win.wait_for(lambda: log().count('Options screen left') == count, process, timeout=10)

            key(ENTER, 3, GAME_KEY)  # START: the main menu.
            game_options(1)
            key(LEFT, .5)  # The music volume, a step down.
            shot('options-changed')
            leave_options(1)
            shot('options-closed', 3)
            game_options(2)
            leave_options(2)
            shot('options-closed-again', 3)
            # The F1 menu keeps the menus' own panel.
            shown = log().count('Game menu opened')
            key(F1, 0)  # The menu bar; Settings is focused.
            win.wait_for(lambda: log().count('Game menu opened') > shown, process, timeout=5)
            time.sleep(.3)
            key(ENTER, 0)
            shot('f1-settings', SETTLED)
            key(ESCAPE, .5)
            key(ESCAPE, .5)  # Closes the menu bar.
            game_options(3, '720p')
            for tab in ('game', 'display', 'quality', 'controls', 'advanced'):
                key(PAGE_DOWN, 0)
                if tab == 'game':
                    shot('720p-tab-switch-a', .05)
                    shot('720p-tab-switch-b', .1)
                shot(f'720p-{tab}', SETTLED)
                if tab == 'controls':
                    key(ENTER, 1)  # Configure controls, first of the tab.
                    shot('720p-controls-page')
                    key(ESCAPE, 1)
                    shot('720p-controls-back')
            for _ in range(8):
                key(DOWN, .15)
            shot('720p-advanced-end', .5)
            for _ in range(3):  # The game's options, Game, Display.
                key(PAGE_DOWN, SETTLED)
            key(DOWN, .3)
            shot('720p-display-focus')
            key(ESCAPE, 0)
            shot('720p-close-a', .1)
            shot('720p-close-b', .15)
            win.wait_for(lambda: log().count('Options screen left') == 3, process, timeout=10)
            shot('720p-closed', 3)
            for count, (name, size) in enumerate((('1080p', (1920, 1080)), ('wide', (2560, 1080))), 4):
                resize(*size)
                game_options(count, name)
                leave_options(count)
                time.sleep(3)
            text = log()
            music = [int(line.split(' music ')[1].split()[0]) for line in text.splitlines()
                     if 'Options screen hidden for the settings' in line]
            assert len(music) == 5 and music[1] == max(music[0] - 1, 0) and len(set(music[1:])) == 1, music
            assert 'the art of the game' not in text, 'the game art did not load'
            # A normal program close also exercises the deferred native path.
            win.user32.PostMessageW(hwnd, 0x10, 0, 0)
            process.wait(timeout=25)
            assert process.returncode == 0, process.returncode
            shutil.copy2(max((product/'data/logs').glob('*.log'), key=lambda p: p.stat().st_mtime), output/(api+'.log'))
            print(json.dumps(dict(api=api, passed=True, exit=process.returncode)), flush=True)
        finally:
            if process.poll() is None:
                hwnd = win.window(process.pid)
                if hwnd:
                    win.user32.PostMessageW(hwnd, 0x10, 0, 0)
                try:
                    process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == '__main__':
    main()
