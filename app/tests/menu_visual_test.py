"""Capture native menu pages for review against FigCraft v4, including 21:9.

Usage: python -I app/tests/menu_visual_test.py <product folder> <game.iso> <new output folder>
Uses isolated configuration and saves. Does not change the supplied product.
"""
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

spec = importlib.util.spec_from_file_location('setup_test', Path(__file__).with_name('setup_boot_test.py'))
win = importlib.util.module_from_spec(spec)
spec.loader.exec_module(win)


def main():
    source, iso, output = (Path(arg).resolve() for arg in sys.argv[1:])
    output.mkdir(parents=True, exist_ok=False)
    for api in ('d3d12', 'vulkan'):
        product = Path(tempfile.mkdtemp(prefix='narutorise-visual-'))
        for item in source.iterdir():
            if item.name == 'narutorise.exe' or item.suffix.lower() == '.dll':
                shutil.copy2(item, product/item.name)
        config = dict(renderer='native', native_backend=api, fullscreen=False, skip_intro_videos=True,
                      frame_rate=60, mnk_mode=True, mnk_mouse=False, custom_game_root=str(iso),
                      launcher_language='pt_BR', check_for_updates=False, show_fps_overlay=True)
        (product/'narutorise.toml').write_text('\n'.join(f'{key} = {json.dumps(value)}' for key,value in config.items()), encoding='utf-8')
        process = subprocess.Popen([str(product/'narutorise.exe')], cwd=product, env=dict(os.environ, REX_USER_DATA_ROOT=str(product/'user')))
        def log():
            files = list((product/'logs').glob('*.log'))
            return max(files, key=lambda p:p.stat().st_mtime).read_text(encoding='utf-8', errors='replace') if files else ''
        try:
            hwnd = win.wait_for(lambda:win.window(process.pid), process)
            win.wait_for(lambda:'Native renderer backend:' in log() and not (product/'user/launch.marker').exists(), process)
            def key(value):
                scan = win.user32.MapVirtualKeyW(value, 0)
                flags = 1 | (scan << 16) | ((1 << 24) if 0x21 <= value <= 0x28 else 0)
                win.user32.PostMessageW(hwnd, 0x100, value, flags)
                win.user32.PostMessageW(hwnd, 0x101, value, flags | 0xc0000000)
                time.sleep(.15)
            def controls_click(dx,dy):
                rect=win.w.RECT();win.user32.GetClientRect(hwnd,ctypes.byref(rect))
                ratio=max(1,rect.bottom/720);width=min(1040,rect.right/ratio-48);height=min(586.667,rect.bottom/ratio-32)
                x=round((rect.right/ratio-width)/2*ratio+dx*ratio);y=round((rect.bottom/ratio-height)/2*ratio+dy*ratio)
                xy=(y<<16)|x
                win.user32.PostMessageW(hwnd,0x200,0,xy);win.user32.PostMessageW(hwnd,0x201,1,xy);win.user32.PostMessageW(hwnd,0x202,0,xy)
                time.sleep(.2)
            def shot(name):
                time.sleep(.15)
                win.capture(hwnd, output/(api+'-'+name+'.png'))
            time.sleep(4.3); shot('hint')
            time.sleep(2); key(0x70); shot('overlay')
            key(13); key(0x22)
            for _ in range(3): key(0x28)
            shot('display')
            key(0x22); key(13); shot('quality-values'); key(27)
            # Escape returns to the bar. Reopen the Controls tab.
            key(13)
            for _ in range(3): key(0x22)
            shot('controls')
            key(0x7a); time.sleep(.5); shot('controls-wide') # F11 uses the real fullscreen path.
            key(13); shot('devices-wide')
            controls_click(150,194); shot('mapping-wide')
            controls_click(150,281); shot('movement-camera-wide')
            key(27); key(27); key(27); key(0x27); key(13); shot('dlc-wide')
            key(27); key(0x70); key(0x70)
            key(0x27); key(0x27); key(13); shot('exit-wide')
            # A normal program close also exercises the deferred native path.
            win.user32.PostMessageW(hwnd, 0x10, 0, 0)
            process.wait(timeout=20); assert process.returncode == 0
            shutil.copy2(max((product/'logs').glob('*.log'), key=lambda p:p.stat().st_mtime), output/(api+'.log'))
            print(json.dumps(dict(api=api, passed=True, exit=process.returncode)), flush=True)
        finally:
            if process.poll() is None:
                hwnd = win.window(process.pid)
                if hwnd: win.user32.PostMessageW(hwnd, 0x10, 0, 0)
                try: process.wait(timeout=20)
                except subprocess.TimeoutExpired: process.kill(); process.wait()


if __name__ == '__main__':
    main()
