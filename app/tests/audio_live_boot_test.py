"""Check the audio options of the overlay applied live, in an isolated running product.

Usage: python -I app/tests/audio_live_boot_test.py <product> <game root> <new output> [d3d12|vulkan]

Through F1 > Settings: the master volume from 100 to 50 (Game tab), stereo to
5.1, then the audio layer to SDK and back to native (Advanced tab). Each step
is checked in the saved TOML, in the log and in a loopback recording of the
default output device (soundcard). The game takes the screen and plays audio.
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

import numpy as np
import soundcard as sc


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


win = load('setup_helpers', 'setup_boot_test.py')
focus = load('focus_helpers', 'close_boot_test.py')


def level(seconds):
    speaker = sc.default_speaker()
    mic = sc.get_microphone(id=str(speaker.name), include_loopback=True)
    with mic.recorder(samplerate=48000, channels=2) as recorder:
        data = recorder.record(numframes=int(48000 * seconds)).astype(np.float64)
    return float(20 * np.log10(np.sqrt((data ** 2).mean()) + 1e-12))


def main():
    source, game, output = (Path(value).resolve() for value in sys.argv[1:4])
    api = sys.argv[4] if len(sys.argv) > 4 else 'vulkan'
    output.mkdir(parents=True, exist_ok=False)
    product = Path(tempfile.mkdtemp(prefix='narutorise-audio-live-'))
    for item in source.iterdir():
        if item.name == 'narutorise.exe' or item.suffix.lower() == '.dll':
            shutil.copy2(item, product / item.name)
    (output / 'identity.json').write_text(json.dumps(dict(source=str(source), product=str(product), api=api,
        exe_sha256=hashlib.sha256((product / 'narutorise.exe').read_bytes()).hexdigest()), indent=2))
    (product / 'data').mkdir()
    config_path = product / 'data/narutorise.toml'
    config = dict(renderer='native', native_backend=api, fullscreen=False, skip_intro_videos=True,
                  show_startup_menu=False, check_for_updates=False, crash_reporting=False,
                  crash_reporting_asked=True, custom_game_root=str(game), audio_layer='native',
                  audio_channels='stereo', master_volume=1.0)
    config_path.write_text('\n'.join(f'{k} = {json.dumps(v)}' for k, v in config.items()), encoding='utf-8')
    process = subprocess.Popen([str(product / 'narutorise.exe')], cwd=product,
                               env=dict(os.environ, REX_DATA_ROOT=str(product / 'data'),
                                        REX_USER_DATA_ROOT=str(product / 'user')))
    checks = []

    def log():
        logs = list((product / 'data/logs').glob('*.log'))
        return max(logs, key=lambda p: p.stat().st_mtime).read_text(encoding='utf-8', errors='replace') if logs else ''

    def saved():
        return tomllib.loads(config_path.read_text(encoding='utf-8'))

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

        def lines(text):
            return log().count(text)

        for attempt in range(10):
            try:
                focus.focus_window(hwnd)
                break
            except AssertionError:
                if attempt == 9 or process.poll() is not None: raise
                time.sleep(.5)
        full = level(6)
        check('native-output-plays', full > -60 and 'audio endpoint' not in log(), level_db=round(full, 1))
        opened = lines('Game menu opened;')
        key(0x70)
        win.wait_for(lambda: lines('Game menu opened;') > opened, process)
        key(13, 1.0)
        for _ in range(4): key(0x28)
        win.capture(hwnd, output / 'game-tab-audio.png')
        # One step per key press; a press the window drops is repeated.
        for _ in range(80):
            if 'master volume 0.50' in log(): break
            key(0x25, .15)
        win.wait_for(lambda: 'master volume 0.50' in log(), process, timeout=10)
        check('volume-saved-and-applied', saved().get('master_volume') == 0.5 and 'master volume 0.50' in log(),
              saved=saved().get('master_volume'))
        half = level(6)
        check('volume-lowers-the-output', -18 < half - full < -6, full_db=round(full, 1), half_db=round(half, 1))

        reopened = lines('[narutorise] Audio device')
        key(0x28)
        key(0x27, 1.0)
        win.wait_for(lambda: lines('[narutorise] Audio device') > reopened, process, timeout=10)
        device = [line for line in log().splitlines() if '[narutorise] Audio device' in line][-1]
        check('surround-saved-and-applied', saved().get('audio_channels') == 'surround' and 'sending 5.1' in device,
              device=device.split('] ', 4)[-1])
        surround = level(6)
        check('surround-plays', surround > -60, level_db=round(surround, 1))

        for _ in range(4): key(0x22)
        for _ in range(7): key(0x28)
        win.capture(hwnd, output / 'advanced-tab-audio.png')
        key(0x27, 1.0)
        win.wait_for(lambda: 'moved to the SDK layer' in log(), process, timeout=10)
        check('sdk-layer-saved-and-applied', saved().get('audio_layer') == 'sdk' and 'audio endpoint' in log())
        sdk = level(6)
        check('sdk-layer-plays-at-the-volume', -6 < sdk - half < 6, sdk_db=round(sdk, 1), half_db=round(half, 1))
        key(0x25, 1.0)
        win.wait_for(lambda: 'moved to the native layer' in log(), process, timeout=10)
        back = level(6)
        check('native-layer-back', saved().get('audio_layer') == 'native' and back > -60, level_db=round(back, 1))

        key(27)
        focus.alt_f4(hwnd)
        process.wait(timeout=30)
        text = log()
        report = [line for line in text.splitlines() if '[narutorise] Audio (end)' in line]
        check('clean-close-with-report', process.returncode == 0 and report, exit_code=process.returncode,
              report=report[-1].split('] ', 4)[-1] if report else None)
    finally:
        if process.poll() is None:
            process.kill()
        (output / 'narutorise.log').write_text(log(), encoding='utf-8')
        shutil.rmtree(product, ignore_errors=True)
    print(json.dumps(dict(passed=all(c['passed'] for c in checks), checks=len(checks))))


if __name__ == '__main__':
    main()
