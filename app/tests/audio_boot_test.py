"""Boot tests of the native audio (app/src/audio): the game with its own audio output.

Usage:
  python -I app/tests/audio_boot_test.py <product folder> <game> <new output folder>
         [--backend d3d12|vulkan] [--failure-runs N] [--hold SECONDS]
         [--cases native,native-xma,native-failure,sdk] [--capture] [--replay FILE] [--env KEY=VALUE ...]

Cases, each in new data, cache and temp folders, with crash reporting off:
  native         the default device; the callback and frames reach the device; the
                 XMA is native too: the title's XAudio finds the contexts through
                 our registers and nothing is refused (the title screen plays no
                 XMA);
  native-xma     as native, with the keyboard as the controller: Start, then
                 Fight Mode and VS Mode, whose sounds are XMA voices: contexts are
                 created and kicked and frames decoded, without error;
  native-failure SDL_AUDIO_DRIVER names a driver SDL does not have, so the device
                 never opens (the stage 0 crash): the game reaches its first frame,
                 runs on the mute device and closes normally, --failure-runs times
                 (the native XMA is checked as in the native case);
  sdk            NARUTORISE_AUDIO_BACKEND=sdk: the SDK's audio, as before.
Each boot waits for the game's first frame, keeps it open for --hold seconds and
closes the window (WM_CLOSE). --capture records the native device stream of the
native case to capture.wav in its folder (NARUTORISE_AUDIO_CAPTURE) and reports
its gaps; --replay plays an input recording (REX_NATIVE_INPUT_REPLAY) in every
boot. The output folder holds game logs and recordings: keep it out of the
repository. Windows only.
"""
import argparse
import array
import ctypes
import importlib.util
from ctypes import wintypes
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ALLOWED_ENV = {'SYSTEMROOT', 'WINDIR', 'PATH', 'COMSPEC', 'PATHEXT', 'OS',
               'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE'}
FORCED_DRIVER = 'narutorise-forced-failure'
# The title's render driver callback, from the static study (sub_8215AEE8).
EXPECTED_CALLBACK = '0x8215AEE8'
# native-xma: from the title screen into VS Mode's player selection (key, hold ms, wait s after).
MENU_KEYS = [(0x0D, 150, 4), (0x32, 120, 1), (0x20, 120, 2), (0x32, 120, 1), (0x20, 120, 3)]  # Start, d-pad down, A
MENU_FLAGS = ['--mnk_mode=true', '--mnk_mouse=false']


def close_window(pid):
    user = ctypes.windll.user32

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def close(hwnd, _):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid:
            user.PostMessageW(hwnd, 0x10, 0, 0)
        return True
    user.EnumWindows(close, 0)


def press(pid, keys):
    """Keys to the game's window, as the keyboard-as-controller reads them."""
    spec = importlib.util.spec_from_file_location('close_boot_test', Path(__file__).with_name('close_boot_test.py'))
    focus = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(focus)
    user = ctypes.windll.user32
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def find(hwnd, _):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True
    user.EnumWindows(find, 0)
    if not found:
        return False
    focus.focus_window(found[0])
    for key, hold, wait in keys:
        scan = user.MapVirtualKeyW(key, 0)
        user.PostMessageW(found[0], 0x100, key, 1 | (scan << 16))
        time.sleep(hold / 1000)
        user.PostMessageW(found[0], 0x101, key, 1 | (scan << 16) | 0xC0000000)
        time.sleep(wait)
    return True


def terminate(pid):
    handle = ctypes.windll.kernel32.OpenProcess(1, False, pid)
    if handle:
        ctypes.windll.kernel32.TerminateProcess(handle, 124)
        ctypes.windll.kernel32.CloseHandle(handle)


def read_json(path):
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except (OSError, json.JSONDecodeError):
        return None


def presented(session):
    path = session / 'events.jsonl'
    if not path.exists():
        return False
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get('operation') == 'first-game-present' and event.get('state') == 'end':
            return True
    return False


def boot(executable, game, folder, backend, extra_env, hold, keys=None, flags=(), timeout=180):
    folder.mkdir()
    temporary = folder / 'temp'
    temporary.mkdir()
    data = folder / 'data'
    env = {key: value for key, value in os.environ.items() if key.upper() in ALLOWED_ENV}
    env.update(TEMP=str(temporary), TMP=str(temporary), **extra_env)
    command = [str(executable), '--data_root=' + str(data), '--cache_root=' + str(folder / 'cache'),
               '--game_data_root=' + str(game), '--renderer=native', '--native_backend=' + backend,
               '--show_startup_menu=false', '--setup=false', '--check_for_updates=false',
               '--crash_reporting=false', '--skip_intro_videos=true', *flags]
    started = time.monotonic()
    process = subprocess.Popen(command, cwd=folder, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    child = None
    present_at = None
    closed = False
    pressed = None
    while process.poll() is None:
        sessions = list((data / 'incidents').glob('session-*'))
        if sessions:
            if child is None:
                child = (read_json(sessions[0] / 'manifest.json') or {}).get('child_pid')
            if present_at is None and presented(sessions[0]):
                present_at = time.monotonic()
        if keys and present_at and pressed is None and child and time.monotonic() - present_at >= 8:
            pressed = press(child, keys)
        if present_at and not closed and time.monotonic() - present_at >= hold and child:
            close_window(child)
            closed = True
        if time.monotonic() - started > timeout:
            if child:
                terminate(child)
            break
        time.sleep(0.1)
    try:
        stdout, stderr = process.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        process.kill()
        stdout, stderr = process.communicate()
    (folder / 'terminal.log').write_bytes(stdout + stderr)
    sessions = sorted((data / 'incidents').glob('session-*'))
    assert len(sessions) == 1, (folder, 'sessions', len(sessions))
    manifest = read_json(sessions[0] / 'manifest.json') or {}
    log = (sessions[0] / 'session.log').read_text(encoding='utf-8', errors='replace')
    return {'folder': str(folder), 'presented': present_at is not None, 'closed_by_test': closed, 'pressed': pressed,
            'classification': manifest.get('classification'), 'exit': manifest.get('raw_exit_hex'),
            'seconds': round(time.monotonic() - started, 1), 'log': log}


def audio_json(log, prefix):
    """The JSON of the last '[narutorise] Audio<prefix>: {...}' line."""
    found = None
    for line in log.splitlines():
        match = re.search(r'\[narutorise\] Audio' + re.escape(prefix) + r': (\{.*\})\s*$', line)
        if match:
            found = json.loads(match.group(1))
    return found


def capture_gaps(path):
    """Silent runs inside the recorded stream: runs of at least 10 ms of exact
    zeros between sound, which a dropped or late frame leaves."""
    raw = path.read_bytes()
    channels = int.from_bytes(raw[22:24], 'little')
    samples = array.array('f')
    samples.frombytes(raw[44:44 + (len(raw) - 44) // 4 * 4])
    frames = len(samples) // channels
    silent = [all(samples[i * channels + c] == 0.0 for c in range(channels)) for i in range(frames)]
    first = next((i for i, s in enumerate(silent) if not s), frames)
    last = frames - next((i for i, s in enumerate(reversed(silent)) if not s), frames)
    gaps, run = [], 0
    for i in range(first, last):
        if silent[i]:
            run += 1
        else:
            if run >= 480:
                gaps.append({'at_s': round((i - run) / 48000, 3), 'ms': round(run / 48, 1)})
            run = 0
    peak = max((abs(v) for v in samples), default=0.0)
    return {'seconds': round(frames / 48000, 1), 'channels': channels, 'sound_from_s': round(first / 48000, 2),
            'peak': round(peak, 4), 'silent_runs_10ms': len(gaps), 'runs': gaps[:20]}


def check(condition, message, failures):
    if not condition:
        failures.append(message)


def judge_xma(log, failures, played):
    """The native XMA's counters at the close ('[narutorise] Audio XMA: {...}');
    `played`: the title went where XMA voices play."""
    xma = audio_json(log, ' XMA')
    check(xma is not None, "the 'Audio XMA' counters at the close", failures)
    if not xma:
        return {}
    registers = xma['registers']
    check(registers['reads_600'] == 1, "the title's XAudio read the contexts' address from our 0x600 once", failures)
    check(xma['exhausted'] == 0, 'no context refused', failures)
    if played:
        check(xma['allocations'] >= 1, 'the title created contexts', failures)
        check(xma['kicks'] >= 1 and registers['kick_writes'] >= 1, 'the title kicked contexts through our registers',
              failures)
        check(xma['frames'] >= 1 and xma['blocks'] >= 1, 'frames were decoded and written', failures)
    errors = {key: xma[key] for key in ('decode_failures', 'bad_data', 'bad_addresses', 'unknown_contexts',
                                        'bad_buffers', 'outside_shape')}
    errors.update(reads_other=registers['reads_other'], writes_other=registers['writes_other'],
                  writes_601=registers['writes_601'])
    check(not any(errors.values()), f'no XMA error, refusal or unknown register ({errors})', failures)
    keys = ('allocations', 'kicks', 'frames', 'blocks', 'loops', 'streams_ended', 'kick_us_mean', 'kick_us_max',
            'kicks_over_1ms', 'kicks_over_budget')
    return {key: xma[key] for key in keys}


def judge(case, result):
    failures = []
    log = result['log']
    check(result['presented'], 'the game reached its first frame', failures)
    check(result['classification'] == 'normal-exit' and result['exit'] == '0x00000000',
          f"normal exit (got {result['classification']} {result['exit']})", failures)
    check(result['closed_by_test'], 'closed by the test, not by itself', failures)
    fatal = [line for line in log.splitlines() if 'Unhandled guest access violation' in line or '[FATAL]' in line]
    check(not fatal, f'no fatal line ({fatal[:1]})', failures)
    backend = 'sdk' if case == 'sdk' else 'native'
    check(f'[narutorise] Audio backend: {backend}' in log, f'the log names the {backend} backend', failures)
    sdk_audio_lines = [line for line in log.splitlines() if '] [apu] ' in line]
    summary = audio_json(log, '')
    running = audio_json(log, ' running')
    if case == 'sdk':
        check(summary == {'backend': 'sdk'} or summary is None or summary.get('backend') == 'sdk',
              'no native audio summary beyond the backend', failures)
        check(any('audio endpoint' in line for line in sdk_audio_lines), "the SDK's audio opened its endpoint",
              failures)
        check(audio_json(log, ' XMA') is None, 'no native XMA with the SDK backend', failures)
        return failures, {'sdk_audio_lines': [line.split('] ', 4)[-1] for line in sdk_audio_lines[:3]]}
    check(not sdk_audio_lines, f"the SDK's audio stays unused ({sdk_audio_lines[:1]})", failures)
    check(running is not None, "the 'Audio running' counters after about 5 s", failures)
    check(summary is not None, "the 'Audio' counters at the close", failures)
    if not summary:
        return failures, {}
    check(summary['registers'] >= 1 and summary['register_refused'] == 0, 'the title registered its client',
          failures)
    check(summary['first_callback'] == EXPECTED_CALLBACK,
          f"the callback is sub_8215AEE8 (got {summary['first_callback']})", failures)
    check(summary['callbacks'] > 0 and summary['submits'] > 0, 'the callback ran and frames were submitted', failures)
    check(summary['frames_invalid'] == 0 and summary['submit_unknown'] == 0, 'no bad frame or handle', failures)
    check(summary['thread_started'] and summary['thread_joined'] and summary['shut_down'],
          'the callback thread started, stopped and was joined at the close', failures)
    layout = summary['layout']
    check(layout['implausible'] == 0, f'every frame looks like 6 big-endian float planes ({layout})', failures)
    if case in ('native', 'native-xma'):
        check(summary['device']['kind'] == 'sdl' and summary['device_opens'] == 1 and summary['open_failures'] == 0,
              f"the device opened once ({summary['device']}, {summary['last_open_error']})", failures)
        check(summary['frames_to_device'] > 0 and summary['device_lost'] == 0, 'frames reached the device', failures)
        check(layout['plausible'] > 0, 'some frames carried sound', failures)
        in_play = summary['queue_empty_periods'] - summary['queue_empty_closing']
        check(in_play == 0, f'the device never ran dry during play ({in_play} empty periods)', failures)
    else:
        check(summary['device']['kind'] == 'mute' and summary['open_failures'] >= 1, 'the mute device stood in',
              failures)
        check(FORCED_DRIVER in summary['last_open_error'], 'the open error names the forced driver', failures)
        check(summary['frames_to_device'] == 0 and summary['frames_muted'] > 0, 'frames were dropped, counted',
              failures)
    if case == 'native-xma':
        check(result['pressed'], 'the keys reached the window', failures)
    xma = judge_xma(log, failures, case == 'native-xma')
    keys = ('registers', 'unregisters', 'first_callback', 'first_argument', 'callbacks', 'periods',
            'double_periods', 'skipped_periods', 'grid_restarts', 'queue_empty_periods', 'queue_empty_closing', 'queue_min', 'submits', 'frames_to_device', 'frames_muted',
            'frames_invalid', 'device_opens', 'open_failures', 'last_open_error', 'device_lost', 'device_stalls')
    facts = {key: summary[key] for key in keys}
    facts['device'] = summary['device']
    facts['layout'] = layout
    facts['xma'] = xma
    return failures, facts


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('product', type=Path)
    parser.add_argument('game', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--backend', default='d3d12', choices=('d3d12', 'vulkan'))
    parser.add_argument('--failure-runs', type=int, default=3)
    parser.add_argument('--hold', type=float, default=20)
    parser.add_argument('--cases', default='native,native-xma,native-failure,sdk')
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--replay', type=Path)
    parser.add_argument('--env', action='append', default=[], help='KEY=VALUE for every boot (repeatable)')
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('This driver closes the window with WM_CLOSE and runs on Windows only')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    executable = args.product.resolve() / 'narutorise.exe'
    game = args.game.resolve()
    cases = set(args.cases.split(','))
    plan = [('native', {})] if 'native' in cases else []
    plan += [('native-xma', {})] if 'native-xma' in cases else []
    plan += [('native-failure', {'SDL_AUDIO_DRIVER': FORCED_DRIVER})] * (args.failure_runs if 'native-failure' in cases else 0)
    plan += [('sdk', {'NARUTORISE_AUDIO_BACKEND': 'sdk'})] if 'sdk' in cases else []
    results, failed = [], 0
    for index, (case, env) in enumerate(plan, 1):
        folder = output / f'{index:02d}-{case}'
        env = dict(env)
        if args.replay:
            env['REX_NATIVE_INPUT_REPLAY'] = str(args.replay.resolve())
        for pair in args.env:
            key, _, value = pair.partition('=')
            env[key] = value
        if args.capture and case == 'native':
            env['NARUTORISE_AUDIO_CAPTURE'] = str(folder / 'capture.wav')
            env['NARUTORISE_AUDIO_CAPTURE_SECONDS'] = str(int(args.hold + 60))
        menu = case == 'native-xma'
        result = boot(executable, game, folder, args.backend, env, args.hold + (15 if menu else 0),
                      MENU_KEYS if menu else None, MENU_FLAGS if menu else ())
        failures, facts = judge(case, result)
        if args.capture and case == 'native':
            wav = folder / 'capture.wav'
            check(wav.exists(), 'the capture was written', failures)
            if wav.exists():
                facts['capture'] = capture_gaps(wav)
        failed += bool(failures)
        print(f"{'PASS' if not failures else 'FAIL'}: {case} ({result['seconds']} s)"
              + ''.join(f'\n  - {failure}' for failure in failures), flush=True)
        if facts:
            print('  ' + json.dumps(facts), flush=True)
        results.append({'case': case, 'failures': failures, 'facts': facts,
                        **{k: v for k, v in result.items() if k != 'log'}})
    (output / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print(f'{len(results) - failed} of {len(results)} boots passed; results in {output / "results.json"}')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
