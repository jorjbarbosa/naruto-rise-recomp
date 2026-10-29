"""Supervisor regression using disposable child processes and isolated roots."""
import argparse
import json
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    results = []
    for mode in ('normal', 'exit', 'guest', 'failure', 'blocked', 'snapshot', 'stderr', 'concurrent'):
        data = root / mode / 'data 日本語'
        process = subprocess.run([str(args.fixture.resolve()), '--data_root=' + str(data),
                                  '--fixture=' + mode], capture_output=True, timeout=45)
        manifests = list((data / 'incidents').glob('session-*/manifest.json'))
        assert len(manifests) == 1, (mode, process.returncode, process.stderr)
        manifest = json.loads(manifests[0].read_text(encoding='utf-8'))
        assert manifest['state'] == 'complete', (mode, manifest)
        if mode == 'exit':
            assert manifest['raw_exit_code'] == 77, manifest
            assert manifest['classification'] == 'interruption-undetermined', manifest
        elif mode == 'failure':
            assert manifest['raw_exit_code'] == 88, manifest
            assert manifest['classification'] == 'explicit-failure', manifest
        else:
            assert process.returncode == 0, (mode, process.returncode, manifest)
        if mode in ('failure', 'blocked'):
            assert manifest['first_failure']['data']['operation'] == 'original-gpu-operation', manifest
        if mode in ('snapshot', 'blocked'):
            assert manifest['snapshots'], manifest
            # A snapshot is evidence from a live process; normal return proves
            # collection did not terminate the child.
            if __import__('os').name == 'nt':
                assert manifest['snapshots'][0]['present'], manifest
                snapshot = manifests[0].parent / manifest['snapshots'][0]['path']
                assert snapshot.read_bytes()[:4] == b'MDMP', snapshot
        if mode == 'stderr':
            assert manifest['stderr_truncated'], manifest
            assert (manifests[0].parent / 'stderr.log').stat().st_size == 2 * 1024 * 1024
        if mode == 'concurrent':
            assert manifest['progress']['compiler']['count'] == 4000, manifest
        if mode == 'guest':
            guest = manifest['guest_context']['data']
            assert guest['native_pc'] == 0x2222 and guest['access_operation'] == 'write', guest
            assert guest['gpr'][1] == 0x7010fc00 and len(guest['gpr']) == 32, guest
            # CTR and r14-r31 are locals of the recompiled functions: recorded as unavailable.
            assert guest['gpr'][13] == 0x80010000 and guest['gpr'][14] is None and guest['ctr'] is None, guest
            assert (manifests[0].parent / 'guest-context.json').is_file()
        # Every persisted line must be valid JSON even under concurrent writes.
        for line in (manifests[0].parent / 'events.jsonl').read_text(encoding='utf-8').splitlines():
            json.loads(line)
        assert 'dump_absent_reason' in manifest, manifest
        results.append({'case': mode, 'exit_code': manifest['raw_exit_code'],
                        'classification': manifest['classification'], 'manifest': str(manifests[0])})
    (root / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print('PASS: 8 supervisor cases; Unicode, first failure, refreshed guest state, live snapshots, stderr bounds, concurrent events')


if __name__ == '__main__':
    main()
