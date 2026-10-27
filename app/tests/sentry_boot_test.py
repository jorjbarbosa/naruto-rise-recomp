"""Exercise the real app's Sentry initialization with isolated data and cache.

Usage: python sentry_boot_test.py <product-directory> <new-output> [--crash]
The error/crash cases send verification events to this build's Sentry project.
No game data is needed, and the normal user profile is not used for storage.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('product', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--crash', action='store_true')
    args = parser.parse_args()
    product, output = args.product.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / 'empty-game').mkdir()
    (output / 'temp').mkdir()
    # Never inherit credentials into a process whose memory may be dumped.
    allowed = {'SYSTEMROOT', 'WINDIR', 'PATH', 'COMSPEC', 'PATHEXT', 'OS',
               'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE', 'DISPLAY',
               'WAYLAND_DISPLAY', 'XDG_RUNTIME_DIR'}
    env = {key: value for key, value in os.environ.items() if key.upper() in allowed}
    env.update(TEMP=str(output / 'temp'), TMP=str(output / 'temp'), SENTRY_ENVIRONMENT='sentry-setup')
    executable = product / ('narutorise.exe' if os.name == 'nt' else 'narutorise')
    results = []
    cases = [('disabled', False, 'error'), ('error', True, 'error')]
    if args.crash:
        cases.append(('crash', True, 'crash'))
    for name, consent, mode in cases:
        directory = output / name
        directory.mkdir()
        data = directory / 'data 日本語'
        cache = directory / 'cache 日本語'
        process = subprocess.run([str(executable), '--data_root=' + str(data),
                                  '--cache_root=' + str(cache),
                                  '--game_data_root=' + str(output / 'empty-game'),
                                  '--crash_reporting=' + str(consent).lower(),
                                  '--sentry_test=' + mode], cwd=output, env=env,
                                 capture_output=True, timeout=60)
        (directory / 'terminal.log').write_bytes(process.stdout + process.stderr)
        logs = list((data / 'logs').glob('*.log'))
        text = '\n'.join(path.read_text(encoding='utf-8', errors='replace') for path in logs)
        event = re.search(r'Verification event ([0-9a-f-]{36}): Naruto Rise Sentry verification error', text)
        if name == 'disabled':
            assert process.returncode == 2, (name, process.returncode)
            assert '[sentry] Reporting disabled' in text, text
            assert not (cache / 'sentry').exists(), 'Opt-out created a reporting database'
        elif name == 'error':
            assert process.returncode == 0, (name, process.returncode, text)
            assert event, text
            assert (cache / 'sentry').is_dir(), 'Missing isolated reporting database'
        else:
            assert process.returncode != 0, 'Verification abort did not terminate the process'
            assert 'Triggering requested verification crash' in text, text
        results.append({'case': name, 'exit_code': process.returncode,
                        'event_id': event.group(1).replace('-', '') if event else None,
                        'logs': [str(path.relative_to(output)) for path in logs]})
    (output / 'result.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print(json.dumps(results, indent=2))
    print('PASS: real app initialization; consent gate and verification trigger')


if __name__ == '__main__':
    main()
