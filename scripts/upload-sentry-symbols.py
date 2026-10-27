"""Upload only the selected Naruto Rise build's native debug artifacts to Sentry.

Requires sentry-cli and SENTRY_AUTH_TOKEN in the local/CI environment. Does not
upload source files. Never searches other builds or uploads game data.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path, help='The exact build that will be distributed')
    parser.add_argument('--org', default='naruto-rise')
    parser.add_argument('--project', default='narutorise')
    parser.add_argument('--cli', default='sentry-cli')
    parser.add_argument('--dry-run', action='store_true', help='List selected files; do not upload')
    args = parser.parse_args()
    build = args.build.resolve(strict=True)
    if not build.is_dir():
        parser.error('build must be a directory')
    binaries = [build / name for name in ('narutorise.exe', 'narutorise', 'rexruntime.dll',
                                          'librexruntime.so', 'rexgpu-xenos.dll', 'sentry.dll',
                                          'crashpad_handler.exe', 'crashpad_handler', 'crashpad_wer.dll')]
    binaries += list(build.glob('narutorise_ai2c*.dll')) + list(build.glob('libnarutorise_ai2c*.so'))
    binaries = sorted({path for path in binaries if path.is_file()})
    if not any(path.name in ('narutorise.exe', 'narutorise') for path in binaries):
        parser.error('No Naruto Rise application in the selected build')
    files = set(binaries)
    for binary in binaries:
        # Upload matching sidecars and the actual binaries (unwind information).
        files.update(path for path in (binary.with_suffix('.pdb'), Path(str(binary) + '.debug'),
                                       binary.with_suffix('.debug')) if path.is_file())
    if (build / 'narutorise.exe').is_file() and not (build / 'narutorise.pdb').is_file():
        parser.error('Missing narutorise.pdb; link this Windows build with debug information first')
    files = sorted(files)
    print(f'Target: {args.org}/{args.project}')
    for path in files:
        print(path.name)
    if args.dry_run:
        return
    if not os.environ.get('SENTRY_AUTH_TOKEN'):
        parser.error('Set SENTRY_AUTH_TOKEN in the local/CI environment before uploading')
    cli = shutil.which(args.cli)
    if not cli:
        parser.error('sentry-cli is not installed; use --cli to select its executable')
    subprocess.run([cli, 'debug-files', 'upload', '--org', args.org, '--project', args.project,
                    '--no-sources', '--wait', *map(str, files)], check=True)


if __name__ == '__main__':
    main()
