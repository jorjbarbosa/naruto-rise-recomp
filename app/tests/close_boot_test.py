"""Alt+F4 must close outside SDL's event-watch callback, including with MnK.

Usage: python -I app/tests/close_boot_test.py <product folder> <game.iso> <new output folder>
Starts only isolated copies. Sends the real Alt+F4 shortcut to each test window.
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
win.user32.SetForegroundWindow.argtypes = [win.w.HWND]
win.user32.GetForegroundWindow.restype = win.w.HWND
win.user32.AttachThreadInput.argtypes = [win.w.DWORD, win.w.DWORD, win.w.BOOL]
win.user32.ShowWindow.argtypes = [win.w.HWND, ctypes.c_int]
win.user32.BringWindowToTop.argtypes = [win.w.HWND]


class Keyboard(ctypes.Structure):
    _fields_ = [('vk', win.w.WORD), ('scan', win.w.WORD), ('flags', win.w.DWORD),
                ('time', win.w.DWORD), ('extra', ctypes.c_size_t)]


class Input(ctypes.Structure):
    class Union(ctypes.Union):
        _fields_ = [('key', Keyboard), ('size', ctypes.c_byte * 32)]
    _anonymous_ = ('data',)
    _fields_ = [('type', win.w.DWORD), ('data', Union)]


def focus_window(hwnd):
    for _ in range(3):
        for up in (False, True):
            event = Input(type=1)
            event.key = Keyboard(0x12, win.user32.MapVirtualKeyW(0x12, 0), 2 if up else 0, 0, 0)
            assert win.user32.SendInput(1, ctypes.byref(event), ctypes.sizeof(event)) == 1
        foreground = win.user32.GetForegroundWindow()
        own = ctypes.windll.kernel32.GetCurrentThreadId()
        other = win.user32.GetWindowThreadProcessId(foreground, None) if foreground else 0
        attached = other != own and other and win.user32.AttachThreadInput(own, other, True)
        try:
            win.user32.ShowWindow(hwnd, 9)
            win.user32.BringWindowToTop(hwnd)
            win.user32.SetForegroundWindow(hwnd)
        finally:
            if attached: win.user32.AttachThreadInput(own, other, False)
        time.sleep(.15)
        if win.user32.GetForegroundWindow() == hwnd: return
    title = ctypes.create_unicode_buffer(256)
    win.user32.GetWindowTextW(hwnd, title, 256)
    raise AssertionError(f'could not focus test window {hwnd} ({title.value}); foreground={win.user32.GetForegroundWindow()}; no shortcut was sent')


def alt_f4(hwnd):
    focus_window(hwnd)
    events = (Input * 4)()
    index = 0
    for key, up in ((0x12, False), (0x73, False), (0x73, True), (0x12, True)):
        events[index].type = 1
        events[index].key = Keyboard(key, win.user32.MapVirtualKeyW(key, 0), 2 if up else 0, 0, 0)
        index += 1
    assert win.user32.SendInput(4, events, ctypes.sizeof(Input)) == 4


def main():
    source, iso, output = (Path(arg).resolve() for arg in sys.argv[1:])
    output.mkdir(parents=True, exist_ok=False)
    results = []
    for api in ('d3d12', 'vulkan'):
        for page in ('game', 'overlay', 'settings', 'controls', 'mapping'):
            product = Path(tempfile.mkdtemp(prefix='narutorise-close-'))
            for item in source.iterdir():
                if item.name == 'narutorise.exe' or item.suffix.lower() == '.dll':
                    shutil.copy2(item, product/item.name)
            config = dict(renderer='native', native_backend=api, fullscreen=False, skip_intro_videos=True,
                          frame_rate=60, mnk_mode=True, mnk_mouse=True, custom_game_root=str(iso), check_for_updates=False,
                          show_startup_menu=False, crash_reporting_asked=True)
            (product / 'data').mkdir(exist_ok=True)
            (product/'data/narutorise.toml').write_text('\n'.join(f'{key} = {json.dumps(value)}' for key,value in config.items()), encoding='utf-8')
            process = subprocess.Popen([str(product/'narutorise.exe')], cwd=product,
                                       env=dict(os.environ, REX_DATA_ROOT=str(product / "data"), REX_USER_DATA_ROOT=str(product/'user')))
            def log():
                files = list((product/'data/logs').glob('*.log'))
                return max(files, key=lambda p:p.stat().st_mtime).read_text(encoding='utf-8', errors='replace') if files else ''
            try:
                hwnd = win.wait_for(lambda:win.window(process.pid), process)
                win.wait_for(lambda:'Native renderer backend:' in log() and not (product/'data/launch.marker').exists(), process)
                time.sleep(2)
                def key(value):
                    scan = win.user32.MapVirtualKeyW(value, 0)
                    flags = 1 | (scan << 16) | ((1 << 24) if 0x21 <= value <= 0x28 else 0)
                    win.user32.PostMessageW(hwnd, 0x100, value, flags)
                    win.user32.PostMessageW(hwnd, 0x101, value, flags | 0xc0000000)
                    time.sleep(.15)
                if page != 'game':
                    key(0x70)
                if page in ('settings', 'controls', 'mapping'):
                    key(13)
                if page in ('controls', 'mapping'):
                    for _ in range(3): key(0x22)
                if page in ('controls', 'mapping'):
                    key(13)  # Dedicated four-port configuration.
                if page == 'mapping':
                    rect=win.w.RECT();win.user32.GetClientRect(hwnd,ctypes.byref(rect))
                    ratio=max(1,rect.bottom/720);width=min(1040,rect.right/ratio-48);height=min(586.667,rect.bottom/ratio-32)
                    def click(dx,dy):
                        x=round((rect.right/ratio-width)/2*ratio+dx*ratio);y=round((rect.bottom/ratio-height)/2*ratio+dy*ratio)
                        xy=(y<<16)|x
                        win.user32.PostMessageW(hwnd,0x200,0,xy);win.user32.PostMessageW(hwnd,0x201,1,xy);win.user32.PostMessageW(hwnd,0x202,0,xy)
                        time.sleep(.2)
                    click(150,194);click(926,183)  # Close while capturing in the row.
                time.sleep(.2)
                win.capture(hwnd, output/(api+'-'+page+'.png'))
                started = time.monotonic()
                alt_f4(hwnd)
                process.wait(timeout=20)
                assert process.returncode == 0, process.returncode
                assert 'Title terminated; hard-exiting process.' in log()
                elapsed = time.monotonic() - started
                name = api+'-'+page
                shutil.copy2(max((product/'data/logs').glob('*.log'), key=lambda p:p.stat().st_mtime), output/(name+'.log'))
                result = dict(api=api, page=page, passed=True, exit=process.returncode, close_seconds=round(elapsed,3))
                results.append(result)
                (output/'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
                print(json.dumps(result), flush=True)
            finally:
                if process.poll() is None:
                    process.kill(); process.wait()
    print('PASS: real Alt+F4 closes ten native/MnK configurations', flush=True)


if __name__ == '__main__':
    main()
