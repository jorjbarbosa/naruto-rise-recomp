"""Exercise missing Crashpad and WER files in isolated copies of a player package."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('product', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('WER coverage is Windows-specific')
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    (root / 'game').mkdir()
    allowed = {'SYSTEMROOT', 'WINDIR', 'PATH', 'COMSPEC', 'PATHEXT', 'OS',
               'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE'}
    env = {k: v for k, v in os.environ.items() if k.upper() in allowed}
    results = []
    for name, omitted, mode in [('missing-handler', 'crashpad_handler.exe', 'normal'),
                                 ('missing-wer', 'crashpad_wer.dll', 'fast-fail')]:
        folder = root / name
        product = folder / 'product'
        product.mkdir(parents=True)
        for path in args.product.resolve().iterdir():
            if path.is_file() and path.suffix in ('.exe', '.dll') and path.name != omitted:
                shutil.copy2(path, product / path.name)
        shutil.copytree(args.product / 'defaults', product / 'defaults')
        temporary = folder / 'temp'
        temporary.mkdir()
        data = folder / 'data'
        subprocess.run([str(product / 'narutorise.exe'), '--data_root=' + str(data),
                        '--cache_root=' + str(folder / 'cache'), '--game_data_root=' + str(root / 'game'),
                        '--crash_reporting=false', '--incident_test=' + mode],
                       env={**env, 'TEMP': str(temporary), 'TMP': str(temporary)},
                       capture_output=True, timeout=45)
        manifest = next((data / 'incidents').glob('session-*/manifest.json'))
        record = json.loads(manifest.read_text(encoding='utf-8'))
        if name == 'missing-handler':
            assert record['collector'] == 'degraded', record
            assert 'crashpad-handler-missing' in (manifest.parent / 'events.jsonl').read_text(encoding='utf-8')
        else:
            assert 'missing' in record['metadata']['fast_fail_coverage'], record
            if not record['exception_dump_present']:
                assert record['fast_fail_capture'] == 'not-observed', record
        assert record['state'] == 'complete', record
        results.append({'case': name, 'collector': record['collector'], 'manifest': str(manifest)})
        print('PASS:', name, 'capability and missing-dump reason recorded', flush=True)
    (root / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
