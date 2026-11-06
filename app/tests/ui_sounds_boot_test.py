"""Check the game's interface sounds in the in-game settings, in an isolated running product.

Usage: python -I app/tests/ui_sounds_boot_test.py <product> <game root> <new output> [d3d12|vulkan]

On the main menu (which plays only music), through F1 > Settings, with a loopback recording
of the default output device (soundcard) along the actions:
- the game's music at 0: the game's own cursor and confirmation on its main menu, then the
  settings' cursor, changed values, tabs, closing, opening and an action each sound out of
  silence; the settings' cursor and action are the same signal as the game's own sounds;
- the game's music back: the cursor's signal is in the mix above it;
- the game's effects at 0: the settings' cursor does what the game's own cursor does on its
  main menu (both silent, or both heard);
- audio_layer = "sdk": the cursor sounds through the SDK's output too.
The end report counts the sounds the game's sound manager played, by kind, and none refused.
The game takes the screen and plays audio; no other app may play sound meanwhile.
"""
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

import numpy as np
import soundcard as sc

RATE = 48000
F1, ENTER, ESCAPE, PAGE_UP, PAGE_DOWN, LEFT, UP, RIGHT, DOWN = 0x70, 0x0D, 0x1B, 0x21, 0x22, 0x25, 0x26, 0x27, 0x28
# The game's d-pad down and up, A and B with the keyboard (mnk_mode).
GAME_DOWN, GAME_UP, GAME_A, GAME_B = 0x32, 0x31, 0x20, 0x45
STEP = 1.2  # Between two actions: each sound ends before the next action.


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


win = load('setup_helpers', 'setup_boot_test.py')
focus = load('focus_helpers', 'close_boot_test.py')


class Recording:
    """The default output by loopback from start() to stop(), with the time of each action in it."""

    def __init__(self, seconds):
        self.seconds, self.data, self.marks = seconds, None, []

    def start(self):
        speaker = sc.default_speaker()
        mic = sc.get_microphone(id=str(speaker.name), include_loopback=True)
        ready = threading.Event()

        def run():
            with mic.recorder(samplerate=RATE, channels=2) as recorder:
                self.origin = time.perf_counter()
                ready.set()
                self.data = recorder.record(numframes=int(RATE * self.seconds)).astype(np.float64).mean(axis=1)

        self.thread = threading.Thread(target=run)
        self.thread.start()
        ready.wait(10)

    def mark(self, name):
        self.marks.append((name, time.perf_counter() - self.origin))

    def stop(self):
        self.thread.join()

    def peak_db(self, begin, end):
        # The loudest 10 ms of [begin, end) seconds.
        part = self.data[max(0, int(begin * RATE)):max(0, int(end * RATE))]
        windows = part[:len(part) // 480 * 480].reshape(-1, 480)
        return float(20 * np.log10(np.sqrt((windows ** 2).mean(axis=1)).max() + 1e-12)) if len(windows) else -240.0

    def actions(self):
        # Each action: the loudest 10 ms in the 0.7 s after it, and in the 0.5 s before it.
        return [dict(action=name, after_db=round(self.peak_db(at, at + .7), 1),
                     before_db=round(self.peak_db(at - .5, at - .03), 1)) for name, at in self.marks]

    def at(self, name):
        return next(at for mark, at in self.marks if mark == name)

    def sound(self, name):
        # The first 0.12 s of the sound after an action, out of silence.
        part = self.data[int(self.at(name) * RATE):int((self.at(name) + .7) * RATE)]
        loud = np.flatnonzero(np.abs(part) > 10 ** (-70 / 20))
        return part[loud[0]:loud[0] + int(.12 * RATE)] if len(loud) else np.zeros(int(.12 * RATE))

    def match(self, begin, end, template):
        # How much of [begin, end) is the template at its best place: 1 for the same sound alone,
        # near 0 for unrelated sound (normalized cross-correlation).
        part = self.data[max(0, int(begin * RATE)):int(end * RATE) + len(template)]
        if len(part) < len(template) or not template.any(): return 0.0
        n = 1 << int(np.ceil(np.log2(len(part) + len(template))))
        corr = np.fft.irfft(np.fft.rfft(part, n) * np.conj(np.fft.rfft(template, n)), n)[:len(part) - len(template) + 1]
        squares = np.concatenate(([0.0], np.cumsum(part ** 2)))
        energy = np.sqrt(np.maximum(squares[len(template):] - squares[:-len(template)], 0))
        return float((corr / (energy * np.linalg.norm(template) + 1e-12)).max())


def main():
    source, game, output = (Path(value).resolve() for value in sys.argv[1:4])
    api = sys.argv[4] if len(sys.argv) > 4 else 'vulkan'
    output.mkdir(parents=True, exist_ok=False)
    product = Path(tempfile.mkdtemp(prefix='narutorise-ui-sounds-'))
    for item in source.iterdir():
        if item.name == 'narutorise.exe' or item.suffix.lower() == '.dll':
            shutil.copy2(item, product / item.name)
    (output / 'identity.json').write_text(json.dumps(dict(source=str(source), product=str(product), api=api,
        exe_sha256=hashlib.sha256((product / 'narutorise.exe').read_bytes()).hexdigest()), indent=2))
    (product / 'data').mkdir()
    config = dict(renderer='native', native_backend=api, fullscreen=False, skip_intro_videos=True,
                  show_startup_menu=False, check_for_updates=False, crash_reporting=False,
                  crash_reporting_asked=True, custom_game_root=str(game), mnk_mode=True, audio_layer='native',
                  audio_channels='stereo', master_volume=1.0)
    (product / 'data/narutorise.toml').write_text('\n'.join(f'{k} = {json.dumps(v)}' for k, v in config.items()),
                                                 encoding='utf-8')
    process = subprocess.Popen([str(product / 'narutorise.exe')], cwd=product,
                               env=dict(os.environ, REX_DATA_ROOT=str(product / 'data'),
                                        REX_USER_DATA_ROOT=str(product / 'user')))
    checks = []

    def log():
        logs = list((product / 'data/logs').glob('*.log'))
        return max(logs, key=lambda p: p.stat().st_mtime).read_text(encoding='utf-8', errors='replace') if logs else ''

    def check(name, passed, **evidence):
        result = dict(check=name, passed=bool(passed), **evidence)
        checks.append(result)
        (output / 'results.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
        print(json.dumps(result), flush=True)
        if not passed:
            win.capture(hwnd, output / f'{name}.png')
            raise AssertionError(name)

    hwnd = None
    try:
        hwnd = win.wait_for(lambda: win.window(process.pid), process)
        win.wait_for(lambda: '[narutorise] Audio device' in log(), process, timeout=90)
        time.sleep(8)

        def key(value, wait=.2):
            if win.user32.GetForegroundWindow() != hwnd: focus.focus_window(hwnd)
            events = (focus.Input * 2)()
            extended = 1 if 0x21 <= value <= 0x28 else 0
            for i, flags in enumerate((extended, extended | 2)):
                events[i].type = 1
                events[i].key = focus.Keyboard(value, win.user32.MapVirtualKeyW(value, 0), flags, 0, 0)
            assert win.user32.SendInput(2, win.ctypes.byref(events), win.ctypes.sizeof(focus.Input)) == 2
            time.sleep(wait)

        def hold(value, wait):
            # A key for the game (keyboard mode): held long enough for its input to see it.
            if win.user32.GetForegroundWindow() != hwnd: focus.focus_window(hwnd)
            for flags in (0, 2):
                event = focus.Input(type=1)
                event.key = focus.Keyboard(value, win.user32.MapVirtualKeyW(value, 0), flags, 0, 0)
                assert win.user32.SendInput(1, win.ctypes.byref(event), win.ctypes.sizeof(focus.Input)) == 1
                time.sleep(.15)
            time.sleep(wait)

        def open_settings(wait=1.5):
            opened = log().count('Game menu opened;')
            key(F1)
            win.wait_for(lambda: log().count('Game menu opened;') > opened, process)
            key(ENTER, wait)

        def close_settings():
            key(ESCAPE, 1.0)  # The page.
            key(ESCAPE, 1.0)  # The menu bar.

        def game_volume(name, value, direction):
            # The game's volume as the log last set it (10 before any change).
            def current():
                found = re.findall(rf"the game's {name} volume set to (\d+)", log())
                return int(found[-1]) if found else 10
            for _ in range(15):
                if current() == value: break
                key(direction, .15)
            win.wait_for(lambda: current() == value, process, timeout=10)

        def sounded(recording, prefix, quiet):
            # Each action of the recording sounds: out of silence, or above the music.
            results = recording.actions()
            (output / f'{prefix}.json').write_text(json.dumps(results, indent=1), encoding='utf-8')
            np.save(output / f'{prefix}.npy', recording.data.astype(np.float32))
            for result in results:
                # Out of silence: the tail of a longer sound before (the closing) may still be there.
                heard = result['after_db'] > -60 and result['after_db'] > result['before_db'] + (20 if quiet else 3)
                check(f'{prefix}-{result["action"]}', heard, **result)

        for attempt in range(10):
            try:
                focus.focus_window(hwnd)
                break
            except AssertionError:
                if attempt == 9 or process.poll() is not None: raise
                time.sleep(.5)
        hold(13, 5.0)  # START: the main menu.

        # The game's music at 0: the main menu is silent. The game's own cursor and confirmation
        # on its main menu (Story Mode and back: B plays nothing), then the settings.
        open_settings()
        for _ in range(5): key(DOWN)
        game_volume('music', 0, LEFT)
        close_settings()
        time.sleep(2)
        recording = Recording(36)
        recording.start()
        time.sleep(1)
        recording.mark('game-cursor')
        hold(GAME_DOWN, STEP)
        hold(GAME_UP, STEP)
        recording.mark('game-confirm')
        hold(GAME_A, 2.5)
        hold(GAME_B, 2.5)
        open_settings(0)
        time.sleep(2)
        for _ in range(5): key(DOWN)  # The game's music volume.
        time.sleep(STEP)
        # The values: the game's voice volume, 10 to 9 and back.
        for name, value in (('cursor-down', DOWN), ('value-left', LEFT), ('value-right', RIGHT), ('cursor-up', UP),
                            ('tab-next', PAGE_DOWN), ('tab-previous', PAGE_UP)):
            recording.mark(name)
            key(value, STEP)
        recording.mark('closing')
        key(ESCAPE, STEP + .5)  # The closing lasts half a second.
        recording.mark('opening')
        key(ENTER, STEP)
        for _ in range(3): key(PAGE_DOWN, .6)  # The Controls tab: its first row opens the controls.
        time.sleep(STEP)
        recording.mark('action')
        key(ENTER, 2.5)
        key(ESCAPE, 2.5)  # Back from the controls.
        recording.stop()
        sounded(recording, 'quiet', True)
        # The settings' cursor and action are the game's own sounds.
        for ours, theirs in (('cursor-down', 'game-cursor'), ('action', 'game-confirm')):
            same = recording.match(recording.at(ours), recording.at(ours) + .7, recording.sound(theirs))
            check(f'{ours}-is-the-{theirs}', same > .9, match=round(same, 3))
        cursor = recording.sound('cursor-down')

        # The game's music back: the cursor above it.
        for _ in range(2): key(PAGE_DOWN, .6)  # The Game tab.
        time.sleep(1)
        for _ in range(5): key(DOWN)
        game_volume('music', 10, RIGHT)
        time.sleep(2)
        recording = Recording(8)
        recording.start()
        time.sleep(1)
        for _ in range(4):
            recording.mark('cursor')
            key(DOWN if len(recording.marks) % 2 else UP, STEP)
        recording.stop()
        np.save(output / 'music.npy', recording.data.astype(np.float32))
        # The music is as loud as the cursor: the cursor's own signal in the mix, against the music alone.
        for i, (_, at) in enumerate(recording.marks):
            after, before = recording.match(at, at + .7, cursor), recording.match(at - .6, at - .03, cursor)
            check(f'music-cursor-{i}', after > .15 and after > 2 * before, after=round(after, 3), before=round(before, 3))

        # The game's effects at 0, with its music at 0: the settings' cursor and the game's own.
        game_volume('music', 0, LEFT)
        key(DOWN)
        key(DOWN)
        game_volume('effects', 0, LEFT)
        time.sleep(2)
        recording = Recording(12)
        recording.start()
        time.sleep(1)
        for _ in range(2):
            recording.mark('settings-cursor')
            key(DOWN if len(recording.marks) % 2 else UP, STEP)
        close_settings()
        time.sleep(1)
        for _ in range(2):
            recording.mark('game-cursor')
            hold(GAME_DOWN, STEP)
        recording.stop()
        results = recording.actions()
        (output / 'effects-off.json').write_text(json.dumps(results, indent=1), encoding='utf-8')
        ours = max(r['after_db'] for r in results if r['action'] == 'settings-cursor')
        theirs = max(r['after_db'] for r in results if r['action'] == 'game-cursor')
        check('effects-off-as-the-game', (ours > -60) == (theirs > -60), settings_db=ours, game_db=theirs)

        # The SDK's output: the settings' cursor through it.
        open_settings()
        for _ in range(7): key(DOWN)
        game_volume('effects', 10, RIGHT)
        for _ in range(4): key(PAGE_DOWN, .6)  # The Advanced tab.
        for _ in range(7): key(DOWN)
        key(RIGHT, 1.0)
        win.wait_for(lambda: 'moved to the SDK layer' in log(), process, timeout=10)
        time.sleep(2)
        recording = Recording(8)
        recording.start()
        time.sleep(1)
        for _ in range(3):
            recording.mark('cursor')
            key(UP if len(recording.marks) % 2 else DOWN, STEP)
        recording.stop()
        sounded(recording, 'sdk', True)

        key(ESCAPE, 1.0)
        focus.alt_f4(hwnd)
        process.wait(timeout=30)
        report = [line for line in log().splitlines() if '[narutorise] Audio (end)' in line]
        counts = report[-1].split('interface sounds ')[1].split(' ')[0].split('/') if report else []
        refused = int(report[-1].split('closing), ')[1].split(' ')[0]) if report else -1
        check('counted-by-the-game', process.returncode == 0 and len(counts) == 5 and all(int(c) > 0 for c in counts)
              and refused == 0, exit_code=process.returncode, counts=counts, refused=refused)
    finally:
        if process.poll() is None:
            process.kill()
        # The game runs in a child of the launched supervisor: stop every copy of this product.
        subprocess.run(['powershell', '-NoProfile', '-Command',
                        f"Get-Process narutorise -ErrorAction SilentlyContinue | "
                        f"Where-Object {{ $_.Path -like '{product}*' }} | Stop-Process -Force"], check=False)
        (output / 'narutorise.log').write_text(log(), encoding='utf-8')
        shutil.rmtree(product, ignore_errors=True)
    print(json.dumps(dict(passed=all(c['passed'] for c in checks), checks=len(checks))))


if __name__ == '__main__':
    main()
