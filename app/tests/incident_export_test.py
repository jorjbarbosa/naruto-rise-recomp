"""Validate export contents, corruption rejection, recovery and bounded retention."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    executable = str(args.fixture.resolve())
    data = root / 'data 日本語'
    reports = data / 'incidents'
    reports.mkdir(parents=True)
    # Dead supervisors are recovered, not declared crashes. Old complete
    # sessions are pruned; an active supervisor must remain untouched.
    for i in range(36):
        directory = reports / f'session-old-{i:03d}'
        directory.mkdir()
        manifest = {'schema_version': 1, 'session_id': directory.name,
                    'supervisor_pid': 0, 'state': 'complete', 'classification': 'normal-exit'}
        (directory / 'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
    active = reports / 'session-active'
    active.mkdir()
    (active / 'manifest.json').write_text(json.dumps({'schema_version': 1, 'supervisor_pid': os.getpid(), 'state': 'running'}))
    stale = reports / 'session-stale'
    stale.mkdir()
    (stale / 'manifest.json').write_text(json.dumps({'schema_version': 1, 'supervisor_pid': 0, 'state': 'running'}))
    assert subprocess.run([executable, '--data_root=' + str(data)]).returncode == 0
    assert active.is_dir(), 'Retention deleted an active session'
    assert len(list(reports.glob('session-old-*'))) <= 32
    recovered = json.loads((stale / 'manifest.json').read_text())
    assert recovered['classification'] == 'interruption-undetermined', recovered
    actual = next(p for p in reports.glob('session-*/manifest.json') if p.parent.name.startswith('session-1'))
    archive = root / 'export 日本語.zip'
    assert subprocess.run([executable, '--incident-export', str(actual.parent), str(archive)]).returncode == 0
    with zipfile.ZipFile(archive) as package:
        assert package.testzip() is None
        manifest = json.loads(package.read('manifest.json'))
        assert set(package.namelist()) == {'manifest.json', *(a['path'] for a in manifest['artifacts'])}
        for artifact in manifest['artifacts']:
            assert hashlib.sha256(package.read(artifact['path'])).hexdigest() == artifact['sha256']
    # Existing export/temp must never be truncated or removed on rejection.
    sentinel = root / 'existing.zip.partial'
    sentinel.write_bytes(b'keep')
    assert subprocess.run([executable, '--incident-export', str(actual.parent), str(root / 'existing.zip')]).returncode == 2
    assert sentinel.read_bytes() == b'keep'
    source = actual.read_bytes()
    corrupted = json.loads(source)
    corrupted['artifacts'][0]['path'] = '../private-save.dat'
    actual.write_text(json.dumps(corrupted), encoding='utf-8')
    assert subprocess.run([executable, '--incident-export', str(actual.parent), str(root / 'unsafe.zip')]).returncode == 2
    assert not (root / 'unsafe.zip').exists()
    actual.write_bytes(source)
    print('PASS: ZIP CRC/hashes, artifact allowlist, traversal rejection, existing file preservation, recovery, retention and active session protection')


if __name__ == '__main__':
    main()
