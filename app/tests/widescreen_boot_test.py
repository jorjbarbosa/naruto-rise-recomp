"""Boot the game at one screen aspect and capture a fixed route (widescreen mode).

Usage: python -I app/tests/widescreen_boot_test.py <product dir> <game root> <output dir>
         [--aspect=16:9|21:9|32:9|16:10|4:3|monitor] [--api=d3d12|vulkan] [--saves=<user folder>]
         [--route=story|fight|<steps>] [--hud-world=edges|center] [--hud-fight=center|edges]
         [--aspect-ratio=auto|16:9] [--exe=<narutorise.exe>] [--option=<key>=<value> ...]

The product folder gives narutorise.exe and its DLLs (--exe replaces the
executable, for the baseline of the 16:9 comparison). The game runs windowed
with a client of the aspect (window_width/height), the Flash catalog log on
(widescreen_log) and deterministic presentation options. --saves copies a user
folder (a profile with saves) into the isolated product. The route is a comma
list of steps: wait:<s>, waitlog:<text>[:<s>] (until the log has it), shot:<name>,
resize:<w>x<h> (the window's client), key:<name>[*n] (RETURN, SPACE, ESCAPE, E,
UP=1, DOWN=2, LEFT=3, RIGHT=4, W, A, S, D). Captures, the log and a summary go
to the output folder; a later widescreen_compare.py run builds the page.
"""
import ctypes
import hashlib
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


win = load('setup_helpers', 'setup_boot_test.py')
focus = load('focus_helpers', 'close_boot_test.py')

# Window clients; 'monitor' is fullscreen on the monitor itself.
ASPECTS = {'16:9': (1280, 720), '21:9': (1680, 720), '32:9': (2560, 720), '16:10': (1152, 720), '4:3': (960, 720),
           'monitor': (0, 0)}
KEYS = {'RETURN': 0x0D, 'SPACE': 0x20, 'ESCAPE': 0x1B, 'E': 0x45, 'UP': 0x31, 'DOWN': 0x32, 'LEFT': 0x33,
        'RIGHT': 0x34, 'W': 0x57, 'A': 0x41, 'S': 0x53, 'D': 0x44, 'TAB': 0x09, 'F1': 0x70, 'PGDN': 0x22,
        'ARROWDOWN': 0x28}
ROUTES = {
    # Title, main menu (Story Mode), the save warning when it shows, the single player menu, Load Game and a
    # slot, the world, pause. With the warning: S is ignored and SPACE closes it; without it, S selects Load.
    'story': 'waitlog:PressStart:60,wait:4,shot:01-title,key:RETURN,wait:4,shot:02-main-menu,key:SPACE,wait:5,'
             'shot:03-story,key:S,wait:1,key:SPACE,wait:4,shot:04-single-player,key:S,wait:1,key:SPACE,wait:4,'
             'shot:05-load-menu,key:S,wait:1,key:SPACE,wait:4,shot:06-load-confirm,key:SPACE,wait:10,shot:07-loading,wait:30,'
             'shot:08-world,key:SPACE,wait:2,key:SPACE,wait:2,key:SPACE,wait:2,key:SPACE,wait:2,key:SPACE,wait:4,'
             'shot:09-world-later,key:RETURN,wait:4,shot:10-pause,key:S,wait:1,key:SPACE,'
             'wait:4,shot:11-options,key:E,wait:3,shot:12-pause-again,key:RETURN,wait:3,shot:13-after-pause',
    # A save at this screen, then its thumbnail in Load Game (320 x 180, the middle 16:9 of the screen).
    'save': 'waitlog:PressStart:60,wait:4,key:RETURN,wait:4,key:SPACE,wait:5,key:S,wait:1,key:SPACE,wait:4,'
            'key:S,wait:1,key:SPACE,wait:4,key:S,wait:1,key:SPACE,wait:4,key:SPACE,wait:40,key:SPACE,wait:2,'
            'key:SPACE,wait:2,key:SPACE,wait:2,key:SPACE,wait:2,key:SPACE,wait:4,shot:01-world,key:RETURN,wait:4,'
            'shot:02-pause,key:S,wait:1,key:S,wait:1,key:SPACE,wait:4,shot:03-save-menu,key:S,wait:1,key:S,wait:1,'
            'shot:04-save-slot,key:SPACE,wait:3,shot:05-save-confirm,key:SPACE,wait:10,shot:06-saved,key:SPACE,'
            'wait:3,shot:07-after-save,key:E,wait:3,shot:08-back,key:S,wait:1,key:SPACE,wait:4,shot:09-load-menu,'
            'key:S,wait:1,key:S,wait:1,wait:2,shot:10-load-slot',
    # The settings panel over the game (F1, Settings, the Display tab): the widescreen options.
    'settings': 'waitlog:PressStart:60,wait:4,key:F1,wait:3,shot:01-game-menu,key:RETURN,wait:3,shot:02-settings,'
                'key:PGDN,wait:2,shot:03-display,key:ARROWDOWN*9,wait:1,shot:04-display-down',
    # The title screen alone.
    'title': 'waitlog:PressStart:60,wait:4,shot:01-title',
    # A new game: the confirmation, then the opening (Bink videos and the first scene).
    'newgame': 'waitlog:PressStart:60,wait:4,key:RETURN,wait:4,key:SPACE,wait:5,key:SPACE,wait:4,key:SPACE,wait:4,'
               'shot:01-confirm,key:SPACE,wait:6,shot:02-opening,wait:8,shot:03-opening,wait:10,'
               'shot:04-opening,wait:15,shot:05-scene',
    # The screen changed with the game running: the world at 21:9, then 16:9, 32:9 and 4:3 and back.
    'live': 'waitlog:PressStart:60,wait:4,key:RETURN,wait:4,key:SPACE,wait:5,key:S,wait:1,key:SPACE,wait:4,'
            'key:S,wait:1,key:SPACE,wait:4,key:SPACE,wait:4,key:SPACE,wait:40,shot:01-world-start,'
            'resize:1280x720,wait:4,shot:02-world-16x9,resize:2560x720,wait:4,shot:03-world-32x9,'
            'resize:960x720,wait:4,shot:04-world-4x3,key:RETURN,wait:3,shot:05-pause-4x3,resize:1680x720,wait:4,'
            'shot:06-pause-21x9,key:RETURN,wait:3,shot:07-world-21x9',
    # Fight mode from the main menu: each confirmation captured until the fight.
    'fight': 'waitlog:PressStart:60,wait:4,shot:01-title,key:RETURN,wait:4,shot:02-main-menu,key:S,wait:1,'
             'shot:03-fight-mode,key:SPACE,wait:5,shot:04-fight-menu,key:SPACE,wait:5,shot:05-fight-a,key:SPACE,'
             'wait:5,shot:06-fight-b,key:SPACE,wait:5,shot:07-fight-c,key:SPACE,wait:5,shot:08-fight-d,key:SPACE,'
             'wait:15,shot:09-fight-loading,wait:20,shot:10-fight,wait:6,shot:11-fight-later,key:RETURN,wait:4,'
             'shot:12-fight-pause',
}


def option(name, default=None):
    prefix = f'--{name}='
    return next((arg[len(prefix):] for arg in sys.argv[4:] if arg.startswith(prefix)), default)


def main():
    source, game, output = (Path(value).resolve() for value in sys.argv[1:4])
    aspect = option('aspect', '21:9')
    width, height = ASPECTS[aspect]
    api = option('api', 'd3d12')
    route = option('route', 'story')
    steps = ROUTES.get(route, route)
    output.mkdir(parents=True, exist_ok=False)
    product = Path(tempfile.mkdtemp(prefix='narutorise-widescreen-'))
    for item in source.iterdir():
        if item.name == 'narutorise.exe' or item.suffix.lower() == '.dll':
            shutil.copy2(item, product / item.name)
    exe = option('exe')
    if exe: shutil.copy2(exe, product / 'narutorise.exe')
    saves = option('saves')
    if saves: shutil.copytree(saves, product / 'user')
    (product / 'data').mkdir(exist_ok=True)
    config = dict(renderer='native', native_backend=api, fullscreen=aspect == 'monitor',
                  skip_intro_videos=True, show_startup_menu=False, check_for_updates=False, show_fps_overlay=False,
                  crash_reporting=False, crash_reporting_asked=True, custom_game_root=str(game),
                  aspect_ratio=option('aspect-ratio', 'auto'),
                  hud_world=option('hud-world', 'edges'), hud_fight=option('hud-fight', 'center'),
                  widescreen_log=True, swap_post_effect='none', present_effect='bilinear', resolution_scale=1,
                  internal_resolution_scale=0.0, frame_rate=60, vsync=True)
    if width: config.update(window_width=width, window_height=height)
    for extra in (arg[9:] for arg in sys.argv[4:] if arg.startswith('--option=')):
        key, _, value = extra.partition('=')
        try:  # A JSON value (number, true/false, list, quoted string); anything else is the text itself.
            config[key] = json.loads(value)
        except json.JSONDecodeError:
            config[key] = value
    (product / 'data/narutorise.toml').write_text('\n'.join(f'{k} = {json.dumps(v)}' for k, v in config.items()),
                                                  encoding='utf-8')
    identity = dict(source=str(source), product=str(product), aspect=aspect, client=[width, height], api=api,
                    route=route, steps=steps, config=config,
                    exe_sha256=hashlib.sha256((product / 'narutorise.exe').read_bytes()).hexdigest())
    (output / 'identity.json').write_text(json.dumps(identity, indent=2), encoding='utf-8')
    environment = dict(os.environ, REX_DATA_ROOT=str(product / 'data'), REX_USER_DATA_ROOT=str(product / 'user'))
    cache = option('cache')  # A shader cache shared by the runs: later runs skip the compilation.
    if cache:
        Path(cache).mkdir(parents=True, exist_ok=True)
        environment['REX_CACHE_ROOT'] = str(Path(cache).resolve())
    process = subprocess.Popen([str(product / 'narutorise.exe')], cwd=product, env=environment)
    shots, failure = [], None

    def log():
        logs = list((product / 'data/logs').glob('*.log'))
        return max(logs, key=lambda p: p.stat().st_mtime).read_text(encoding='utf-8', errors='replace') if logs else ''

    try:
        hwnd = win.wait_for(lambda: win.window(process.pid), process, timeout=60)
        win.wait_for(lambda: 'Native renderer backend:' in log(), process, timeout=120)
        win.wait_for(lambda: not (product / 'user/launch.marker').exists(), process, timeout=120)
        for attempt in range(10):
            try:
                focus.focus_window(hwnd)
                break
            except AssertionError:
                if attempt == 9 or process.poll() is not None: raise
                time.sleep(.5)

        def key(vk):
            # Held for a few frames: the game polls the emulated controller once a frame.
            if win.user32.GetForegroundWindow() != hwnd: focus.focus_window(hwnd)
            extended = 1 if 0x21 <= vk <= 0x28 else 0
            for flags in (extended, extended | 2):
                event = focus.Input(type=1)
                event.key = focus.Keyboard(vk, win.user32.MapVirtualKeyW(vk, 0), flags, 0, 0)
                assert win.user32.SendInput(1, ctypes.byref(event), ctypes.sizeof(focus.Input)) == 1
                time.sleep(.15)
            time.sleep(.25)

        for step in steps.split(','):
            kind, _, value = step.partition(':')
            if process.poll() is not None: raise AssertionError(f'process exited: {process.returncode}')
            stopped = next((line for line in log().splitlines() if 'Native renderer stopped' in line), None)
            if stopped: raise AssertionError(stopped.strip())
            if kind == 'wait': time.sleep(float(value))
            elif kind == 'waitlog':  # waitlog:<text>[:<seconds>]: a movie or line in the log, else go on.
                text, _, seconds = value.partition(':')
                end = time.monotonic() + float(seconds or 30)
                while time.monotonic() < end and text not in log() and process.poll() is None: time.sleep(.25)
            elif kind == 'shot':
                path = output / f'{value}.png'
                win.capture(hwnd, path)
                shots.append(value)
            elif kind == 'resize':  # resize:<w>x<h>: the client size of the window, the game running.
                width_new, height_new = (int(v) for v in value.split('x'))
                window, client = win.w.RECT(), win.w.RECT()
                win.user32.GetWindowRect(hwnd, ctypes.byref(window))
                win.user32.GetClientRect(hwnd, ctypes.byref(client))
                extra_w = (window.right - window.left) - client.right
                extra_h = (window.bottom - window.top) - client.bottom
                win.user32.SetWindowPos(hwnd, None, 0, 0, width_new + extra_w, height_new + extra_h, 0x0002 | 0x0004)
            elif kind == 'key':
                name, _, count = value.partition('*')
                for _ in range(int(count or 1)): key(KEYS[name])
            else: raise ValueError(step)
    except Exception as error:  # Captured with the evidence; the process is closed below.
        failure = repr(error)
    finally:
        # The game runs as a child of the session supervisor: close the whole tree, then check that no
        # process of this product is left (a leftover game holds the GPU and breaks the next run).
        running = process.poll() is None
        tree = win.process_tree(process.pid)
        subprocess.run(['taskkill', '/F', '/T', '/PID', str(process.pid)], capture_output=True)
        for pid in tree:
            subprocess.run(['taskkill', '/F', '/PID', str(pid)], capture_output=True)
        try: process.wait(15)
        except subprocess.TimeoutExpired: process.kill()
        query = ("Get-CimInstance Win32_Process -Filter \"Name = 'narutorise.exe'\" | "
                 f"Where-Object {{ $_.ExecutablePath -like '{product}*' }} | ForEach-Object {{ $_.ProcessId }}")
        leftovers = []
        for _ in range(20):
            leftovers = subprocess.run(['powershell', '-NoProfile', '-Command', query], capture_output=True,
                                       text=True).stdout.split()
            if not leftovers: break
            for pid in leftovers:
                subprocess.run(['taskkill', '/F', '/PID', pid], capture_output=True)
            time.sleep(.5)
        if leftovers and not failure: failure = f'game processes left: {leftovers}'
        text = log()
        (output / 'narutorise.log').write_text(text, encoding='utf-8')
        # The saves of the run and their thumbnails (widescreen_thumbnail.py), for the 16:9 crop.
        thumbnails = load('thumbnail_helpers', 'widescreen_thumbnail.py')
        for save in sorted((product / 'user').glob('*/555307E5/*/*.ninjasave/Save.dat')):
            data = save.read_bytes()
            dds = thumbnails.find_dds(data)
            if not dds: continue
            name = save.parent.name.split('.')[0]
            (output / 'saves').mkdir(exist_ok=True)
            (output / 'saves' / f'{name}.dat').write_bytes(data)
            thumbnails.write_png(output / f'thumb-{name}.png', *thumbnails.dds_rgb(dds))
        lines = [line for line in text.splitlines() if 'widescreen' in line]
        summary = dict(aspect=aspect, client=[width, height], shots=shots, failure=failure,
                       exited_early=not running, returncode=process.returncode,
                       widescreen=[line for line in lines if 'widescreen:' in line],
                       movies=len([line for line in lines if 'widescreen movie' in line]))
        (output / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
        print(json.dumps(summary, indent=2))
        shutil.rmtree(product, ignore_errors=True)
    return 0 if not failure else 1


if __name__ == '__main__':
    sys.exit(main())
