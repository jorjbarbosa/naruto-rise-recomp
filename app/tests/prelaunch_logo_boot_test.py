"""Windows prelaunch logo checks; never modifies or redistributes game assets.

Usage: python -I app/tests/prelaunch_logo_boot_test.py PRODUCT EXTRACTED_GAME NEW_OUTPUT [ISO]
Builds a test-only XDVDFS container with the local XEX/SRA and placeholder core
files. It is only used for the prelaunch reader, never to execute the game.
All configs/user data/product copies live in an isolated temporary directory.
"""
import importlib.util
import ctypes
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


win = module('setup_boot_test')
focus = module('close_boot_test')


def choose_native_file(process, path):
    """Only address the file-name edit and Open button in this process's dialog."""
    user = win.user32
    user.GetDlgCtrlID.argtypes = [win.w.HWND]
    user.GetDlgItem.argtypes = [win.w.HWND, ctypes.c_int]
    user.GetDlgItem.restype = win.w.HWND
    user.GetClassNameW.argtypes = [win.w.HWND, win.w.LPWSTR, ctypes.c_int]
    user.SendMessageW.argtypes = [win.w.HWND, win.w.UINT, win.w.WPARAM, win.w.LPARAM]
    user.SendMessageW.restype = win.w.LPARAM
    def find():
        dialogs = []
        @ctypes.WINFUNCTYPE(win.w.BOOL, win.w.HWND, win.w.LPARAM)
        def top(hwnd, _):
            owner = win.w.DWORD()
            user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            cls = ctypes.create_unicode_buffer(256)
            user.GetClassNameW(hwnd, cls, 256)
            if owner.value == process.pid and cls.value == '#32770':
                dialogs.append(hwnd)
            return True
        user.EnumWindows(top, 0)
        for dialog in dialogs:
            fields = []
            @ctypes.WINFUNCTYPE(win.w.BOOL, win.w.HWND, win.w.LPARAM)
            def child(hwnd, _):
                cls = ctypes.create_unicode_buffer(256)
                user.GetClassNameW(hwnd, cls, 256)
                if (cls.value == 'Edit' and user.GetDlgCtrlID(hwnd) in (1148, 1152)
                        and user.IsWindowVisible(hwnd) and user.IsWindowEnabled(hwnd)):
                    fields.append(hwnd)
                return True
            user.EnumChildWindows(dialog, child, 0)
            button = user.GetDlgItem(dialog, 1)
            if fields and button:
                return dialog, fields[0], button
        return None
    dialog, field, button = win.wait_for(find, process, timeout=15)
    value = ctypes.create_unicode_buffer(str(path))
    if user.SendMessageW(field, 0x000c, 0, ctypes.cast(value, ctypes.c_void_p).value):
        user.PostMessageW(button, 0x00f5, 0, 0)
        return
    # Modern dialogs can expose a read-only proxy Edit to automation. Use the
    # dialog's File name accelerator only after verifying its foreground HWND.
    focus.focus_window(dialog)
    def keys(values):
        events = (focus.Input * len(values))()
        for i, (vk, scan, flags) in enumerate(values):
            events[i].type = 1
            events[i].key = focus.Keyboard(vk, scan, flags, 0, 0)
        assert user.GetForegroundWindow() == dialog
        assert user.SendInput(len(events), events, ctypes.sizeof(focus.Input)) == len(events)
    keys([(0x12,0,0),(0x4e,0,0),(0x4e,0,2),(0x12,0,2)])  # Alt+N: File name.
    keys([(0x11,0,0),(0x41,0,0),(0x41,0,2),(0x11,0,2)])
    data = str(path).encode('utf-16-le')
    values = []
    for (code,) in struct.iter_unpack('<H', data):
        values += [(0,code,4),(0,code,6)]
    keys(values)
    keys([(13,0,0),(13,0,2)])


def fixture_iso(game, path):
    files = [('default.xex', (game/'default.xex').read_bytes()),
             ('Naruto_clip.SRA', (game/'Naruto_clip.SRA').read_bytes())]
    files += [(name, b'prelaunch fixture only') for name in
              ('ai2c.dll', 'naruto.bf', 'xeshader.sdb', 'soundxe.big', 'soundxe.fat')]
    sector = 2048
    directory = bytearray(b'\xff' * sector)
    entry, at = 0, 34
    payloads = []
    for i, (name, data) in enumerate(files):
        size = (14 + len(name) + 3) // 4 * 4
        struct.pack_into('<HHIIBB', directory, entry, 0, (entry+size)//4 if i+1 < len(files) else 0,
                         at, len(data), 0x20, len(name))
        directory[entry+14:entry+14+len(name)] = name.encode('ascii')
        payloads.append((at, data))
        at += (len(data) + sector - 1) // sector
        entry += size
    with path.open('wb') as f:
        f.truncate(at * sector)
        f.seek(32 * sector)
        f.write(b'MICROSOFT*XBOX*MEDIA' + struct.pack('<II', 33, sector))
        f.seek(33 * sector)
        f.write(directory)
        for at, data in payloads:
            f.seek(at * sector)
            f.write(data)


def main():
    source, game, output = (Path(arg).resolve() for arg in sys.argv[1:4])
    output.mkdir(parents=True, exist_ok=False)
    product = Path(tempfile.mkdtemp(prefix='narutorise-logo-'))
    for item in source.iterdir():
        if item.name == 'narutorise.exe' or item.suffix.lower() == '.dll':
            shutil.copy2(item, product/item.name)
    no_logo = product/'without-art'
    no_logo.mkdir()
    for name in ('default.xex', 'ai2c.dll', 'naruto.bf', 'xeshader.sdb', 'soundxe.big', 'soundxe.fat'):
        actual = next(p for p in game.iterdir() if p.name.lower() == name)
        os.link(actual, no_logo/name)
    iso = Path(sys.argv[4]).resolve() if len(sys.argv) > 4 else product/'prelaunch-fixture.iso'
    if len(sys.argv) <= 4:
        fixture_iso(game, iso)
    results = []
    for api in ('d3d12', 'vulkan'):
        for kind, root in (('folder', game), ('absent-logo', no_logo), ('corrupt-logo', no_logo), ('select-iso', None)):
            if os.environ.get('NARUTORISE_LOGO_TEST_CASE') and kind != os.environ['NARUTORISE_LOGO_TEST_CASE']:
                continue
            name = api+'-'+kind
            if kind == 'absent-logo':
                (no_logo/'Naruto_clip.SRA').unlink(missing_ok=True)
            if kind == 'corrupt-logo':
                (no_logo/'Naruto_clip.SRA').write_bytes(b'FRSA\xff\xff\xff\xff')
            config = dict(renderer='native', native_backend=api, fullscreen=False, input_backend='sdl',
                          launcher_language='pt_BR', check_for_updates=False)
            if root:
                config['custom_game_root'] = str(root)
            (product/'narutorise.toml').write_text('\n'.join(f'{k} = {json.dumps(v)}' for k,v in config.items()), encoding='utf-8')
            before = set((product/'logs').glob('*.log'))
            log = None
            with (output/(name+'.console.log')).open('wb') as console:
                process = subprocess.Popen([str(product/'narutorise.exe'), '--setup'], cwd=product,
                    env=dict(os.environ, REX_USER_DATA_ROOT=str(product/('user-'+name))),
                    stdout=console, stderr=subprocess.STDOUT)
                def text():
                    nonlocal log
                    logs = set((product/'logs').glob('*.log')) - before
                    if logs:
                        log = max(logs, key=lambda p: p.stat().st_mtime)
                    return log.read_text(encoding='utf-8', errors='replace') if log else ''
                try:
                    hwnd = win.wait_for(lambda: win.window(process.pid), process)
                    win.wait_for(lambda: 'Setup screen:' in text(), process)
                    if root is None:
                        time.sleep(.4)
                        assert 'Setup logo: loaded' not in text()
                        win.capture(hwnd, output/(name+'-before.png'))
                        focus.focus_window(hwnd)
                        flags = 1 | (win.user32.MapVirtualKeyW(13, 0) << 16)
                        win.user32.PostMessageW(hwnd, 0x100, 13, flags)
                        win.user32.PostMessageW(hwnd, 0x101, 13, flags | 0xc0000000)
                        choose_native_file(process, iso)
                        win.wait_for(lambda: 'Game selected:' in text(), process)
                    expected = 'Setup logo: unavailable' if 'logo' in kind else 'Setup logo: loaded 528x288'
                    win.wait_for(lambda: expected in text(), process)
                    time.sleep(.4)
                    win.capture(hwnd, output/(name+'.png'))
                    assert 'Native renderer backend:' not in text(), 'prelaunch must not start the guest'
                    win.user32.PostMessageW(hwnd, 0x10, 0, 0)
                    process.wait(timeout=25)
                    assert process.returncode == 0
                    results.append(dict(case=name, passed=True, exit=process.returncode))
                    print(json.dumps(results[-1]), flush=True)
                    (output/'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
                finally:
                    if process.poll() is None:
                        win.user32.PostMessageW(hwnd, 0x10, 0, 0)
                        try:
                            process.wait(timeout=15)
                        except subprocess.TimeoutExpired:
                            process.kill(); process.wait()
                    text()
                    if log:
                        shutil.copy2(log, output/(name+'.log'))
    print('PASS: centered prelaunch, real local logo, native ISO selection, missing/corrupt fallback, both APIs', flush=True)


if __name__ == '__main__':
    main()
