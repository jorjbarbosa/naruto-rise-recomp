"""Windows integration check of the native setup gate and its runtime handover.

Usage: python -I app/tests/setup_boot_test.py <built product folder> <game.iso> <new output folder>
Creates an isolated product/user tree; never edits the supplied product or ISO.
Uses PrintWindow and posted messages; Play uses real keyboard input only after
the driver confirms that the isolated test window owns the foreground.
"""
import ctypes
import importlib.util
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import tempfile
import zlib

user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
user32.GetDC.restype = w.HDC
user32.GetDC.argtypes = [w.HWND]
user32.ReleaseDC.argtypes = [w.HWND, w.HDC]
gdi32.CreateCompatibleDC.restype = w.HDC
gdi32.CreateCompatibleDC.argtypes = [w.HDC]
gdi32.CreateCompatibleBitmap.restype = w.HBITMAP
gdi32.CreateCompatibleBitmap.argtypes = [w.HDC, ctypes.c_int, ctypes.c_int]
gdi32.SelectObject.restype = w.HGDIOBJ
gdi32.SelectObject.argtypes = [w.HDC, w.HGDIOBJ]
gdi32.GetDIBits.argtypes = [w.HDC, w.HBITMAP, w.UINT, w.UINT, ctypes.c_void_p, ctypes.c_void_p, w.UINT]
gdi32.DeleteObject.argtypes = [w.HGDIOBJ]
gdi32.DeleteDC.argtypes = [w.HDC]
user32.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]
user32.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
user32.SetForegroundWindow.argtypes = [w.HWND]
# A private handle: the module is loaded again by the tests that reuse it, and
# the shared windll.kernel32 would keep the argtypes of another load.
kernel32 = ctypes.WinDLL('kernel32')
kernel32.CreateToolhelp32Snapshot.restype = w.HANDLE
kernel32.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
kernel32.CloseHandle.argtypes = [w.HANDLE]


class ProcessEntry(ctypes.Structure):
    _fields_ = [('dwSize', w.DWORD), ('cntUsage', w.DWORD), ('th32ProcessID', w.DWORD),
                ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', w.DWORD), ('cntThreads', w.DWORD),
                ('th32ParentProcessID', w.DWORD), ('pcPriClassBase', w.LONG), ('dwFlags', w.DWORD),
                ('szExeFile', w.WCHAR * 260)]


kernel32.Process32FirstW.argtypes = kernel32.Process32NextW.argtypes = [w.HANDLE, ctypes.POINTER(ProcessEntry)]


def process_tree(pid):
    """pid and its children: the game runs as a child of the session supervisor."""
    tree = {pid}
    snapshot = kernel32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    if snapshot in (None, w.HANDLE(-1).value):
        return tree
    try:
        entry = ProcessEntry(dwSize=ctypes.sizeof(ProcessEntry))
        more = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
        while more:
            if entry.th32ParentProcessID == pid:
                tree.add(entry.th32ProcessID)
            more = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    return tree


def window(pid):
    owners = process_tree(pid)
    found = []
    @ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    def visit(hwnd, _):
        owner = w.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value in owners and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True
    user32.EnumWindows(visit, 0)
    return found[0] if found else None


def wait_for(predicate, process, timeout=25):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        if process.poll() is not None:
            raise AssertionError(f'process exited early: {process.returncode}')
        time.sleep(.05)
    raise AssertionError('timed out')


def capture(hwnd, path):
    rect = w.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    width, height = rect.right, rect.bottom
    dc = user32.GetDC(hwnd)
    memory = gdi32.CreateCompatibleDC(dc)
    bitmap = gdi32.CreateCompatibleBitmap(dc, width, height)
    old = gdi32.SelectObject(memory, bitmap)
    try:
        assert user32.PrintWindow(hwnd, memory, 3)
        pixels = ctypes.create_string_buffer(width * height * 4)
        info = ctypes.create_string_buffer(struct.pack('<IiiHHIIiiII', 40, width, -height, 1, 32, 0, len(pixels), 0, 0, 0, 0))
        assert gdi32.GetDIBits(memory, bitmap, 0, height, pixels, info, 0) == height
        bgra = pixels.raw
        # Opaque PNG; the swapchain's alpha is not meaningful.
        rgb = bytearray(width * height * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = bgra[2::4], bgra[1::4], bgra[0::4]
        scan = b''.join(b'\0' + rgb[y*width*3:(y+1)*width*3] for y in range(height))
        def chunk(name, data):
            return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name+data))
        path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                         chunk(b'IDAT', zlib.compress(scan)) + chunk(b'IEND', b''))
        # F0 v7: the labels are in the center, not the old left column.
        menu = b''.join(rgb[(y*width + width//4)*3:(y*width + width*3//4)*3]
                        for y in range(height*44//100, height*80//100))
        return zlib.crc32(menu)
    finally:
        gdi32.SelectObject(memory, old)
        gdi32.DeleteObject(bitmap)
        gdi32.DeleteDC(memory)
        user32.ReleaseDC(hwnd, dc)


def main():
    source, iso, output = (Path(value).resolve() for value in sys.argv[1:])
    output.mkdir(parents=True, exist_ok=False)
    # Outside the repository: otherwise the app finds the developer game_root.
    product = Path(tempfile.mkdtemp(prefix='narutorise-setup-'))
    for item in source.iterdir():
        if item.name == 'narutorise.exe' or item.suffix.lower() == '.dll':
            shutil.copy2(item, product/item.name)
    results = []
    # Imported lazily: the close driver also imports this module for Win32
    # types, but does not run main. Verify focus before sending Play's Enter.
    focus_spec = importlib.util.spec_from_file_location('close_driver', Path(__file__).with_name('close_boot_test.py'))
    focus = importlib.util.module_from_spec(focus_spec)
    focus_spec.loader.exec_module(focus)

    def case(name, reason=None, api='d3d12', args=(), first=False, failed=False, play=False, language='en', root=iso, show_menu=True):
        user = product / ('user-' + name)
        user.mkdir()
        data = product / 'data'
        data.mkdir(exist_ok=True)
        # The marker lives at the data folder root, which the cases share:
        # each case starts with only the marker it plants.
        marker = data/'launch.marker'
        marker.unlink(missing_ok=True)
        if failed:
            marker.write_text('started\n')
        config = {'renderer': 'native', 'native_backend': api, 'fullscreen': False,
                  'skip_intro_videos': True, 'frame_rate': 60, 'input_backend': 'sdl', 'launcher_language': language,
                  'check_for_updates': False, 'show_startup_menu': show_menu}
        if not first:
            config['custom_game_root'] = str(root)
        (data/'narutorise.toml').write_text('\n'.join(f'{key} = {json.dumps(value, ensure_ascii=False)}' for key,value in config.items()), encoding='utf-8')
        before = set((data/'logs').glob('*.log'))
        # The menu items take the focus on mouseover: a resting cursor over the
        # new window would move it off Play before the Enter.
        user32.SetCursorPos(0, 0)
        process = subprocess.Popen([str(product/'narutorise.exe'), *args], cwd=product,
                                   env=dict(os.environ, REX_DATA_ROOT=str(data), REX_USER_DATA_ROOT=str(user)))
        log = None
        try:
            hwnd = wait_for(lambda: window(process.pid), process)
            def text():
                nonlocal log
                new = set((data/'logs').glob('*.log')) - before
                if new:
                    log = max(new, key=lambda p:p.stat().st_mtime)
                return log.read_text(encoding='utf-8', errors='replace') if log else ''
            if reason:
                wait_for(lambda:f'Setup screen: {reason}' in text(), process)
                time.sleep(.4)
                crc = capture(hwnd, output/(name+'.png'))
                if play:
                    focus.focus_window(hwnd)
                    events = (focus.Input * 2)()
                    for i, flags in enumerate((0, 2)):
                        events[i].type = 1
                        events[i].key = focus.Keyboard(13, user32.MapVirtualKeyW(13, 0), flags, 0, 0)
                    assert user32.SendInput(2, ctypes.byref(events), ctypes.sizeof(focus.Input)) == 2
                    try:
                        wait_for(lambda:not marker.exists() and 'Native renderer backend:' in text(), process)
                    except AssertionError:
                        capture(hwnd, output/(name+'-timeout.png'))
                        if log:
                            shutil.copy2(log, output/(name+'-timeout.log'))
                        raise
            else:
                wait_for(lambda:'Native renderer backend:' in text(), process)
                time.sleep(2)
                assert 'Setup screen:' not in text()
                crc = None
            if '--game_data_root=' + str(root) in args:
                assert marker.exists(), 'CLI root must leave the marker untouched'
            elif play or not reason:
                wait_for(lambda:not marker.exists(), process)
            user32.PostMessageW(hwnd, 0x10, 0, 0)
            process.wait(timeout=25)
            assert process.returncode == 0, process.returncode
            if reason and not any(arg.startswith('--game_data_root=') for arg in args):
                assert not marker.exists()
            record = dict(case=name, passed=True, exit=process.returncode, reason=reason, api=api, screenshot_crc=crc)
            if log:
                shutil.copy2(log, output/(name+'.log'))
            results.append(record)
            print(json.dumps(record), flush=True)
            (output/'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
        finally:
            if process.poll() is None:
                hwnd = window(process.pid)
                if hwnd:
                    user32.PostMessageW(hwnd, 0x10, 0, 0)
                try:
                    process.wait(timeout=25)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()

    case('first', 'first-run', first=True)
    case('missing', 'game-missing', root=product/'absent.iso')
    case('failed', 'start-failed', failed=True)
    for api in ('d3d12', 'vulkan'):
        case('setup-'+api, 'requested', api=api, args=('--setup',), play=True)
    case('default-menu', 'startup')
    case('direct', show_menu=False)
    case('command-line', args=('--game_data_root='+str(iso),), failed=True, show_menu=False)
    # A named game ignores a stale marker (startup, not start-failed) and keeps it.
    case('command-line-menu', 'startup', args=('--game_data_root='+str(iso),), failed=True)
    unicode_root = product/'jogo-ação.iso'
    try:
        os.link(iso, unicode_root)
    except OSError as error:
        print(f'NOT_RUN: Unicode ISO hard link is unavailable: {error}', flush=True)
    else:
        case('unicode-path', 'startup', root=unicode_root)
    for language in ('en', 'pt_BR', 'fr', 'de', 'es', 'it', 'ru'):
        case('language-'+language, 'requested', args=('--setup',), language=language)
    crcs = [r['screenshot_crc'] for r in results if r['case'].startswith('language-')]
    assert len(set(crcs)) == 7, 'the seven language screens must render different text'
    print('PASS: setup gate, handover, CLI bypass, seven rendered languages', flush=True)


if __name__ == '__main__':
    main()
