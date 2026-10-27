"""Check visible hot changes from the overlay in an isolated running product.

Usage: python -I app/tests/live_settings_boot_test.py <product> <game.iso> <new output> [d3d12|vulkan]
Optional: --graphics --saves=<fixture directory> --settle=<seconds>.
REX_NATIVE_INPUT_REPLAY may drive a saved-game fixture before the edits.
"""
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import tomllib
import zlib
import struct
import re


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


win = load('setup_helpers', 'setup_boot_test.py')
focus = load('focus_helpers', 'close_boot_test.py')


def png_pixels(path):
    raw = path.read_bytes()
    at, compressed = 8, b''
    while at < len(raw):
        length = struct.unpack_from('>I', raw, at)[0]
        kind, data = raw[at+4:at+8], raw[at+8:at+8+length]
        if kind == b'IHDR': width, height = struct.unpack_from('>II', data)
        if kind == b'IDAT': compressed += data
        at += length + 12
    return width, height, zlib.decompress(compressed)


def orange_pixels(path):
    # capture() emits unfiltered RGB rows; inspect the FPS panel's left edge.
    width, height, pixels = png_pixels(path)
    count = 0
    for y in range(8, min(height, 120)):
        for x in range(0, min(width, 5)):
            offset = y * (width * 3 + 1) + 1 + x * 3
            color = pixels[offset:offset+3]
            count += all(abs(a-b) <= 8 for a, b in zip(color, (242, 112, 31)))
    return count


def game_tab_crc(path):
    width, height, pixels = png_pixels(path)
    ratio = max(1, height / 720)
    left = (width - min(1040 * ratio, width - 48 * ratio)) / 2
    top = (height - min(586.667 * ratio, height - 32 * ratio)) / 2
    x0, x1 = int(left + 35*ratio), int(left + 90*ratio)
    y0, y1 = int(top + 12*ratio), int(top + 36*ratio)
    return zlib.crc32(b''.join(pixels[y*(width*3+1)+1+x0*3:y*(width*3+1)+1+x1*3] for y in range(y0,y1)))


def main():
    source, iso, output = (Path(value).resolve() for value in sys.argv[1:4])
    api = sys.argv[4] if len(sys.argv) > 4 else 'd3d12'
    output.mkdir(parents=True, exist_ok=False)
    product = Path(tempfile.mkdtemp(prefix='narutorise-live-settings-'))
    fixture = next((Path(arg[8:]).resolve() for arg in sys.argv[5:] if arg.startswith('--saves=')), None)
    if fixture: shutil.copytree(fixture, product/'user')
    for item in source.iterdir():
        if item.name == 'narutorise.exe' or item.suffix.lower() == '.dll':
            shutil.copy2(item, product/item.name)
    (output/'identity.json').write_text(json.dumps(dict(source=str(source), product=str(product), api=api,
        exe_sha256=hashlib.sha256((product/'narutorise.exe').read_bytes()).hexdigest()), indent=2))
    (product / 'data').mkdir(exist_ok=True)
    config_path = product / 'data/narutorise.toml'
    config = dict(renderer='native', native_backend=api, fullscreen=False, skip_intro_videos=True, show_startup_menu=False,
                  check_for_updates=False, show_fps_overlay=False, custom_game_root=str(iso))
    if '--graphics' in sys.argv[5:]:
        config.update(frame_rate=60, swap_post_effect='none', present_effect='bilinear', anisotropic_override=5,
                      brightness=.5, vsync=True, resolution_scale=1, internal_resolution_scale=0.0,
                      max_frame_latency=1, triple_buffering=False, msaa_samples=0, transparency_antialiasing=False)
    config_path.write_text('\n'.join(f'{k} = {json.dumps(v)}' for k, v in config.items()), encoding='utf-8')
    process = subprocess.Popen([str(product/'narutorise.exe')], cwd=product,
                               env=dict(os.environ, REX_DATA_ROOT=str(product / "data"), REX_USER_DATA_ROOT=str(product/'user')))
    checks = []
    def log():
        logs = list((product/'data/logs').glob('*.log'))
        return max(logs, key=lambda p:p.stat().st_mtime).read_text(encoding='utf-8', errors='replace') if logs else ''
    def saved(): return tomllib.loads(config_path.read_text(encoding='utf-8'))
    def check(name, passed, **evidence):
        result = dict(check=name, passed=bool(passed), **evidence)
        checks.append(result)
        (output/'results.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
        print(json.dumps(result), flush=True)
    try:
        hwnd = win.wait_for(lambda:win.window(process.pid), process)
        win.wait_for(lambda:'Native renderer backend:' in log(), process)
        win.wait_for(lambda:not (product/'user/launch.marker').exists(), process)
        time.sleep(4)
        win.user32.ClientToScreen.argtypes = [win.w.HWND, win.ctypes.POINTER(win.w.POINT)]
        def key(value):
            if win.user32.GetForegroundWindow() != hwnd: focus.focus_window(hwnd)
            rect = win.w.RECT()
            win.user32.GetClientRect(hwnd, win.ctypes.byref(rect))
            point = win.w.POINT(20, max(20, rect.bottom-20))
            win.user32.ClientToScreen(hwnd, win.ctypes.byref(point))
            win.user32.SetCursorPos(point.x, point.y)
            events = (focus.Input * 2)()
            extended = 1 if 0x21 <= value <= 0x28 else 0
            for i, flags in enumerate((extended, extended | 2)):
                events[i].type = 1
                events[i].key = focus.Keyboard(value, win.user32.MapVirtualKeyW(value, 0), flags, 0, 0)
            assert win.user32.SendInput(2, win.ctypes.byref(events), win.ctypes.sizeof(focus.Input)) == 2
            time.sleep(.2)
        def shot(name):
            path = output / (name+'.png')
            win.capture(hwnd, path)
            return orange_pixels(path)
        for attempt in range(10):
            try:
                focus.focus_window(hwnd)
                break
            except AssertionError:
                if attempt == 9 or process.poll() is not None: raise
                time.sleep(.5)
        settle = next((float(arg[9:]) for arg in sys.argv[5:] if arg.startswith('--settle=')), 0)
        if fixture:
            def in_gameplay():
                path = output/'loading.png'; win.capture(hwnd,path)
                width,height,pixels = png_pixels(path)
                count = 0
                for y in range(int(75*height/720),int(100*height/720)):
                    for x in range(int(160*width/1280),int(275*width/1280)):
                        at = y*(width*3+1)+1+x*3
                        r,g,b = pixels[at:at+3]
                        count += r>170 and 65<g<180 and b<80
                return count > 100
            win.wait_for(in_gameplay,process,timeout=180)
            check('gameplay-fixture-ready',True,pid=process.pid)
        elif settle: time.sleep(settle)
        initial = shot('initial')
        if '--graphics' in sys.argv[5:]:
            # Actual keyboard edits, acknowledged by the frame-boundary owner.
            def selected_tab():
                path = output/'navigation.png'; win.capture(hwnd,path)
                width, height, pixels = png_pixels(path)
                ratio = max(1,height/720)
                left = (width-min(1040*ratio,width-48*ratio))/2
                top = (height-min(586.667*ratio,height-32*ratio))/2
                widths = [80,80,126.667,126.667,120]
                for target in range(5):
                    x = int(left+(20+sum(widths[:target])+widths[target]/2)*ratio)
                    at = int(top+15*ratio)*(width*3+1)+1+x*3
                    if all(abs(a-b)<12 for a,b in zip(pixels[at:at+3],(242,112,31))): return target
                return -1
            def next_tab():
                current = selected_tab()
                assert current >= 0, 'No selected tab visible'
                key(0x22)
                # PgDn is deferred by RmlUi. Wait for its actual frame before
                # sending another; several events in one frame can coalesce.
                win.wait_for(lambda:selected_tab() == (current+1)%5,process)
            def select(target,row):
                next_tab()
                while selected_tab() != target: next_tab()
                for _ in range(row): key(0x28)
            def applied_lines():
                pattern = r'Live settings revision (\d+): applied=(true|false), frame_rate=(\d+), anisotropy=(-?\d+), transparency_aa=(true|false), (\{.*\})'
                return list(re.finditer(pattern, log()))
            def edit(target, row, name, expected, direction=0x27, **runtime):
                select(target, row)
                previous = len(applied_lines())
                key(direction)
                win.wait_for(lambda:len(applied_lines()) > previous, process)
                result = applied_lines()[-1]
                request_key = 'resolution_scale' if name == 'internal_resolution_scale' else name
                win.wait_for(lambda:f'Live setting confirmed: {request_key} (revision {result[1]})' in log(), process)
                detail = json.loads(result[6])
                if name == 'frame_rate':
                    win.wait_for(lambda:f'Frame rate: {expected} (interpolation clock reset)' in log(), process)
                check('live-'+name+'-'+str(expected), result[2] == 'true' and saved().get(name) == expected and
                      all(detail.get(k) == v for k,v in runtime.items()) and process.poll() is None,
                      revision=int(result[1]), engine=detail, pid=process.pid)
                shot('live-'+name+'-'+str(expected))
            opened = log().count('Game menu opened;')
            key(0x70)
            win.wait_for(lambda:log().count('Game menu opened;')>opened,process)
            key(13)
            win.wait_for(lambda:selected_tab()>=0,process)
            edit(1, 2, 'internal_resolution_scale', 1.5, scale=[1.5,1.5])
            edit(1, 2, 'internal_resolution_scale', 0.0, direction=0x25, scale=[1,1])
            edit(1, 3, 'brightness', .51, brightness=.51)
            edit(1, 3, 'brightness', .5, direction=0x25, brightness=.5)
            edit(1, 4, 'frame_rate', 120)
            edit(1, 4, 'frame_rate', 30)
            edit(1, 4, 'frame_rate', 60)
            edit(1, 5, 'vsync', False, vsync=False)
            edit(1, 5, 'vsync', True, vsync=True)
            edit(2, 0, 'msaa_samples', 8, msaa_samples=8)
            edit(2, 0, 'msaa_samples', 0, direction=0x25, msaa_samples=0)
            edit(2, 1, 'swap_post_effect', 'fxaa', post_effect=1)
            edit(2, 1, 'swap_post_effect', 'none', direction=0x25, post_effect=0)
            edit(2, 2, 'present_effect', 'cas', effect=1)
            edit(2, 2, 'present_effect', 'fsr', effect=2)
            edit(2, 2, 'present_effect', 'bilinear', effect=0)
            edit(2, 3, 'anisotropic_override', 1)
            edit(2, 3, 'anisotropic_override', 5, direction=0x25)
            edit(2, 4, 'transparency_antialiasing', True)
            edit(2, 4, 'transparency_antialiasing', False)
            if api == 'd3d12':
                edit(4, 0, 'max_frame_latency', 2, latency=2)
                edit(4, 0, 'max_frame_latency', 1, direction=0x25, latency=1)
            edit(4, 1, 'triple_buffering', True, buffers=3)
            edit(4, 1, 'triple_buffering', False, buffers=2)
            select(0, 2); key(0x27)
            win.wait_for(lambda:saved().get('skip_intro_videos') is False, process)
            key(0x25)
            win.wait_for(lambda:saved().get('skip_intro_videos') is True, process)
            check('intro-option-saved-live', process.poll() is None, pid=process.pid)
            key(27); key(0x70)
        key(0x70); key(13)
        for _ in range(4): key(0x22)
        key(0x28); key(0x28); key(0x27)
        win.wait_for(lambda:saved().get('show_fps_overlay') is True, process)
        shot('fps-setting-on')
        key(27)
        bar = shot('fps-top-bar')
        key(0x70)
        time.sleep(.7)
        game = shot('fps-game')
        check('fps-becomes-visible-without-restart', game > initial + 20,
              initial_orange=initial, bar_orange=bar, game_orange=game, pid=process.pid)
        key(0x70); key(13)
        for _ in range(4): key(0x22)
        key(0x28); key(0x28); key(0x25)
        shot('fps-setting-off')
        win.wait_for(lambda:saved().get('show_fps_overlay') is False, process)
        key(27); key(0x70)
        time.sleep(.3)
        hidden = shot('fps-off')
        check('fps-hides-without-restart', hidden <= initial + 5, orange=hidden)

        def size():
            rect = win.w.RECT()
            win.user32.GetClientRect(hwnd, win.ctypes.byref(rect))
            return rect.right, rect.bottom
        original_size = size()
        key(0x70); key(13); key(0x22)
        for _ in range(6): key(0x28)
        key(0x27)
        win.wait_for(lambda:saved().get('fullscreen') is True and size() != original_size, process)
        check('fullscreen-applies-without-restart', process.poll() is None,
              original_size=original_size, fullscreen_size=size(), pid=process.pid)
        key(0x25)
        win.wait_for(lambda:saved().get('fullscreen') is False and size() == original_size, process)
        check('windowed-mode-restored-without-restart', True, size=size())

        key(0x21)
        for _ in range(3): key(0x28)
        shot('language-en')
        english = game_tab_crc(output/'language-en.png')
        key(0x27)
        win.wait_for(lambda:saved().get('launcher_language') == 'pt_BR', process)
        shot('language-pt')
        portuguese = game_tab_crc(output/'language-pt.png')
        check('menu-language-repaints-without-restart', english != portuguese,
              english_tab_crc=english, portuguese_tab_crc=portuguese)
        key(0x25)

        for _ in range(3): key(0x22)
        key(0x28); key(0x28); key(13); key(0x71)
        win.wait_for(lambda:saved().get('bind_game_menu') == 'F2', process)
        key(27)
        before_close = log().count('Game menu closed;')
        key(0x70)
        check('old-menu-key-is-inactive', log().count('Game menu closed;') == before_close)
        key(0x71)
        win.wait_for(lambda:log().count('Game menu closed;') > before_close, process)
        before_open = log().count('Game menu opened;')
        key(0x70)
        check('old-menu-key-does-not-reopen', log().count('Game menu opened;') == before_open)
        key(0x71)
        win.wait_for(lambda:log().count('Game menu opened;') > before_open, process)
        check('new-menu-key-applies-without-restart', True, pid=process.pid)
        key(13)
        for _ in range(3): key(0x22)
        key(0x28); key(0x28); key(13); key(0x70)
        win.wait_for(lambda:saved().get('bind_game_menu') == 'F1', process)
        key(0x70)
        win.user32.PostMessageW(hwnd, 0x10, 0, 0)
        process.wait(timeout=25)
        check('clean-close', process.returncode == 0, exit=process.returncode)
    except Exception as error:
        check('execution-completed', False, error=str(error))
        raise
    finally:
        if process.poll() is None:
            hwnd = win.window(process.pid)
            if hwnd: win.user32.PostMessageW(hwnd, 0x10, 0, 0)
            try: process.wait(timeout=25)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
        if (product/'data/logs').exists():
            for item in (product/'data/logs').glob('*.log'): shutil.copy2(item, output/item.name)
    assert all(c['passed'] for c in checks), checks


if __name__ == '__main__': main()
