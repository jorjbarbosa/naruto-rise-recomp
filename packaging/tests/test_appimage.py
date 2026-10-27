#!/usr/bin/env python3
"""Bounded AppImage checks with isolated user directories and no game launch."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

image = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[2]
work = Path(tempfile.mkdtemp(prefix='naruto-appimage-check-',
                             dir=sys.argv[2] if len(sys.argv) > 2 else None))
home = work / 'usuário com espaços'
home.mkdir()
env = {**os.environ, 'HOME': str(home), 'XDG_CACHE_HOME': str(home / 'cache'),
       'XDG_CONFIG_HOME': str(home / 'config'), 'XDG_STATE_HOME': str(home / 'state'),
       'XDG_DATA_HOME': str(home / 'data')}
report = {'appimage': str(image), 'work': str(work), 'checks': {}}

def run(command, label, **kwargs):
    result = subprocess.run(command, env=kwargs.pop('env', env), capture_output=True,
                            timeout=40, **kwargs)
    (work / (label + '.log')).write_bytes(result.stdout + result.stderr)
    return result

def check(condition, label):
    if not condition:
        raise AssertionError(label)
    report['checks'][label] = 'PASS'

first = run([str(image), '--appimage-extract-and-run', '--test-audio-wait'], 'runtime-extract-run')
check(first.returncode == 0 and b'PASS:' in first.stdout, 'actual runtime and bundled audio wait test')
check(not (home / 'config/narutorise/narutorise.toml').exists(), 'audio probe does not create app configuration')
second = run([str(image), '--test-audio-wait'], 'runtime-fuse')
report['fuse_execution'] = {'exit_code': second.returncode}
if second.returncode != 0:
    second = run([str(image), '--appimage-extract-and-run', '--test-audio-wait'], 'runtime-second-extract-run')
check(second.returncode == 0 and b'PASS:' in second.stdout, 'second invocation')
check(not list((home / 'cache/narutorise/appimage').glob('*/.ready')), 'no payload deployment in cache')
check(not (home / 'data/narutorise/narutorise.toml').exists(), 'audio probe has no storage side effects')

extract = work / 'extracted'
extract.mkdir()
result = run([str(image), '--appimage-extract'], 'extract', cwd=extract)
check(result.returncode == 0, 'AppImage extraction')
appdir = extract / 'squashfs-root'
metadata = json.loads((appdir / 'packaging-info.json').read_text())
check(metadata.get('payload_execution') == 'appdir', 'image declares direct payload execution')
payload_root = appdir / 'usr/lib/narutorise'
check((payload_root / 'defaults/narutorise.toml').is_file() and not (payload_root / 'narutorise.toml').exists(), 'factory template is immutable and separate')
check(not any((payload_root / name).exists() for name in ('data', 'logs', 'dlc', 'mods', '.ready')), 'image has no mutable data layout')
for name in ('mod.toml', 'texts.json'):
    check((payload_root / 'defaults/mods/pt-br' / name).read_bytes() == (root / 'mods/pt-br' / name).read_bytes(),
          'curated immutable PT-BR seed ' + name)
for name, expected in metadata['binaries'].items():
    check(hashlib.sha256((appdir / 'usr/lib/narutorise' / name).read_bytes()).hexdigest() == expected,
          'unchanged binary ' + name)
check((appdir / '.DirIcon').is_file() and (appdir / 'narutorise.desktop').is_file(), 'desktop icon and entry')

# Exercise argv, cwd and nearby data discovery using a harmless stub payload.
stub = work / 'stub.AppDir'
payload = stub / 'usr/lib/narutorise'
payload.mkdir(parents=True)
(stub / 'build-id').write_text('wrapper-fixture\n')
(stub / 'AppRun').write_bytes((root / 'packaging/appimage/AppRun').read_bytes())
(stub / 'AppRun').chmod(0o755)
(payload / 'defaults').mkdir()
(payload / 'defaults/narutorise.toml').write_text('renderer = "native"\n')
stub_exe = payload / 'narutorise'
stub_exe.write_text('#!/bin/sh\nprintf "%s\\0" "$@" > "$CAPTURE_ARGS"\npwd > "$CAPTURE_CWD"\n')
stub_exe.chmod(0o755)
cwd = work / 'cwd diferente'
cwd.mkdir()
stub_env = {**env, 'APPDIR': str(stub), 'APPIMAGE': str(work / 'Jogo com espaços.AppImage'),
            'CAPTURE_ARGS': str(work / 'argv'), 'CAPTURE_CWD': str(work / 'cwd')}
arguments = ['--game_data_root=relative game dir', '--user_data_root=save com espaços', 'literal;$value']
result = run([str(stub / 'AppRun'), *arguments], 'explicit-argv', env=stub_env, cwd=cwd)
check(result.returncode == 0, 'wrapper explicit launch')
recorded = (work / 'argv').read_bytes().split(b'\0')[:-1]
check(recorded == [a.encode() for a in arguments], 'argv preserved without shell expansion')
check((work / 'cwd').read_text().strip() == str(cwd), 'relative paths retain original cwd')
result = run([str(stub / 'AppRun'), '--setup'], 'nearby-game', env=stub_env, cwd=cwd)
check(result.returncode == 0, 'wrapper nearby launch')
recorded = (work / 'argv').read_bytes().split(b'\0')[:-1]
check(recorded == [b'--setup'], 'nearby discovery remains in native app without overriding its saved ISO')

report['glibc_versions_used'] = sorted(set(
    __import__('re').findall(r'GLIBC_(\d+\.\d+)', subprocess.check_output(
        ['readelf', '--version-info', str(appdir / 'usr/lib/narutorise/narutorise'), str(appdir / 'usr/lib/narutorise/librexruntime.so')], text=True))),
    key=lambda version: tuple(map(int, version.split('.'))))
(work / 'result.json').write_text(json.dumps(report, indent=2))
print(json.dumps({'status': 'PASS', 'checks': len(report['checks']), 'fuse_execution': report['fuse_execution'],
                  'maximum_glibc_requirement': report['glibc_versions_used'][-1], 'report': str(work / 'result.json')}))
