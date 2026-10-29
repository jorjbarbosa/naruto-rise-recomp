"""Exercise the native overlay, shared pages and delivered guest input on both APIs.

Usage: python -I app/tests/overlay_boot_test.py <product folder> <game.iso> <new output folder>
Runs offline with isolated configuration, saves and input recording. No desktop
shortcut is written; shell links are covered by narutorise_menu_input_test.
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
import tomllib

spec = importlib.util.spec_from_file_location('setup_test', Path(__file__).with_name('setup_boot_test.py'))
win = importlib.util.module_from_spec(spec)
spec.loader.exec_module(win)
win.user32.SetForegroundWindow.argtypes = [win.w.HWND]
focus_spec = importlib.util.spec_from_file_location('close_test', Path(__file__).with_name('close_boot_test.py'))
focus = importlib.util.module_from_spec(focus_spec)
focus_spec.loader.exec_module(focus)


def main():
    source, iso, output = (Path(arg).resolve() for arg in sys.argv[1:])
    output.mkdir(parents=True, exist_ok=False)
    results = []
    for api in ('d3d12', 'vulkan'):
        product = Path(tempfile.mkdtemp(prefix='narutorise-overlay-'))
        for item in source.iterdir():
            if item.name == 'narutorise.exe' or item.suffix.lower() == '.dll':
                shutil.copy2(item, product/item.name)
        user = product/'user'
        record = product/'input.txt'
        config = dict(renderer='native', native_backend=api, fullscreen=False, skip_intro_videos=True, show_startup_menu=False,
                      frame_rate=60, mnk_mode=True, mnk_mouse=False, custom_game_root=str(iso),
                      check_for_updates=False, show_fps_overlay=False, crash_reporting_asked=True)
        (product / 'data').mkdir(exist_ok=True)
        (product/'data/narutorise.toml').write_text('\n'.join(f'{key} = {json.dumps(value)}' for key,value in config.items()), encoding='utf-8')
        process = subprocess.Popen([str(product/'narutorise.exe')], cwd=product,
                                   env=dict(os.environ, REX_DATA_ROOT=str(product / "data"), REX_USER_DATA_ROOT=str(user), REX_NATIVE_INPUT_RECORD=str(record)))
        def log():
            files = list((product/'data/logs').glob('*.log'))
            return max(files, key=lambda p:p.stat().st_mtime).read_text(encoding='utf-8', errors='replace') if files else ''
        def states():
            text = record.read_text(encoding='utf-8') if record.exists() else ''
            return [list(map(int, line.split()[1:])) for line in text.splitlines() if line.startswith('S ')]
        def current():
            state = states()
            return state[-1] if state else None
        try:
            hwnd = win.wait_for(lambda:win.window(process.pid), process)
            win.wait_for(lambda:'Native renderer backend:' in log() and not (product/'data/launch.marker').exists(), process)
            time.sleep(.1)
            win.capture(hwnd, output/(api+'-startup.png'))
            focus.focus_window(hwnd)
            time.sleep(3)
            def down(key):
                if win.user32.GetForegroundWindow() != hwnd:
                    focus.focus_window(hwnd)
                scan = win.user32.MapVirtualKeyW(key, 0)
                flags = 1 | (scan << 16) | ((1 << 24) if 0x21 <= key <= 0x28 else 0)
                win.user32.PostMessageW(hwnd, 0x100, key, flags)
            def up(key):
                scan = win.user32.MapVirtualKeyW(key, 0)
                flags = 1 | (scan << 16) | ((1 << 24) if 0x21 <= key <= 0x28 else 0) | 0xc0000000
                win.user32.PostMessageW(hwnd, 0x101, key, flags)
            def key(value):
                down(value); up(value); time.sleep(.12)
            def shot(name):
                win.capture(hwnd, output/(api+'-'+name+'.png'))
            down(ord('W'))
            before = win.wait_for(lambda:next((s for s in reversed(states()) if s[8] > 16000), None), process)
            key(0x70) # F1 opens, without forwarding the key to the guest.
            win.wait_for(lambda:'Game menu opened;' in log(), process)
            neutral = win.wait_for(lambda:next((s for s in reversed(states()) if s[2] == before[2] and s[1] > before[1] and not any(s[4:])), None), process)
            up(ord('W')) # This release must reach the SDK while the guest is gated.
            shot('overlay')
            key(0x27); key(0x27); key(13); shot('exit-cancel')
            key(13); assert process.poll() is None
            key(0x25); key(0x25); key(13); shot('settings')
            for _ in range(4): key(0x22) # Advanced.
            for _ in range(3): key(0x28) # Past the startup menu, updates and shortcut.
            key(0x27)
            time.sleep(.2)
            shot('fps-option')
            saved = tomllib.loads((product/'data/narutorise.toml').read_text(encoding='utf-8'))
            assert saved['show_fps_overlay'] is True
            key(27); key(0x27); key(13); shot('dlc'); key(27)
            key(0x70)
            win.wait_for(lambda:'Game menu closed;' in log(), process)
            down(ord('D'))
            after = win.wait_for(lambda:next((s for s in reversed(states()) if s[2] == before[2] and s[7] > 16000), None), process)
            assert after[8] == 0, 'W stayed held after its release inside the menu'
            assert after[1] - before[1] > 60, 'the guest must keep polling input while the menu is open'
            up(ord('D'))
            key(0x70); key(13) # Settings.
            for _ in range(3): key(0x22) # Controls.
            for _ in range(2): key(0x28) # Configure controls, backend, then the global menu shortcut.
            key(13); key(0x71) # Capture F2.
            saved = tomllib.loads((product/'data/narutorise.toml').read_text(encoding='utf-8'))
            assert saved['bind_game_menu'] == 'F2'
            key(27); key(0x71) # Back, close with the new key.
            count = log().count('Game menu opened;')
            key(0x70); assert log().count('Game menu opened;') == count
            key(0x71); assert log().count('Game menu opened;') == count + 1
            shot('remapped')
            key(0x27); key(0x27); key(13); key(0x27); key(13)
            process.wait(timeout=25)
            assert process.returncode == 0, process.returncode
            shutil.copy2(record, output/(api+'-input.txt'))
            shutil.copy2(max((product/'data/logs').glob('*.log'), key=lambda p:p.stat().st_mtime), output/(api+'.log'))
            result = dict(api=api, passed=True, exit=process.returncode, guest_reads_while_menu=after[1]-before[1],
                          neutral_packet=neutral[3], resumed_packet=after[3])
            results.append(result)
            print(json.dumps(result), flush=True)
            (output/'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
        finally:
            if process.poll() is None:
                hwnd = win.window(process.pid)
                if hwnd: win.user32.PostMessageW(hwnd, 0x10, 0, 0)
                try: process.wait(timeout=25)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
    print('PASS: overlay, shared settings/DLC, live FPS, remapping, neutral guest input, release and confirmed exit', flush=True)


if __name__ == '__main__':
    main()
