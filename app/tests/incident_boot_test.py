"""Inspect real application dumps in isolated profiles; never upload fixtures."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import struct
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('product', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--cases', default='normal,abort,access-violation,observer-av,worker-av,stack-overflow,fast-fail,stop,shutdown-blocked,hang,exit,off-abort')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / 'empty-game').mkdir()
    received = []

    class Trap(BaseHTTPRequestHandler):
        def do_POST(self):
            received.append(self.path)
            self.send_response(200)
            self.end_headers()
        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Trap)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    allowed = {'SYSTEMROOT', 'WINDIR', 'PATH', 'COMSPEC', 'PATHEXT', 'OS',
               'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE', 'DISPLAY',
               'WAYLAND_DISPLAY', 'XDG_RUNTIME_DIR'}
    env = {key: value for key, value in os.environ.items() if key.upper() in allowed}
    env.update(SENTRY_ENVIRONMENT='incident-verification',
               SENTRY_DSN=f'http://public@127.0.0.1:{server.server_port}/1')
    executable = args.product.resolve() / ('narutorise.exe' if os.name == 'nt' else 'narutorise')
    results = []
    for case in args.cases.split(','):
        folder = output / case
        folder.mkdir()
        temporary = folder / 'temp'
        temporary.mkdir()
        data = folder / 'data 日本語'
        mode = 'abort' if case == 'off-abort' else case
        arguments = [str(executable), '--data_root=' + str(data),
                     '--cache_root=' + str(folder / 'cache 日本語'),
                     '--game_data_root=' + str(output / 'empty-game'),
                     '--crash_reporting=false', '--incident_test=' + mode]
        if case == 'off-abort':
            arguments.append('--log_level=off')
        process = subprocess.run(arguments, cwd=folder, env={**env, 'TEMP': str(temporary), 'TMP': str(temporary)},
                                 capture_output=True, timeout=180 if mode == 'automatic-hang' else 60)
        (folder / 'terminal.log').write_bytes(process.stdout + process.stderr)
        manifests = list((data / 'incidents').glob('session-*/manifest.json'))
        assert len(manifests) == 1, (case, process.returncode, process.stderr)
        path = manifests[0]
        manifest = json.loads(path.read_text(encoding='utf-8'))
        assert manifest['state'] == 'complete', (case, manifest)
        assert manifest['collector'] == 'ready', (case, manifest)
        assert manifest['transport']['state'] == 'local-only', (case, manifest)
        if mode in ('abort', 'access-violation', 'observer-av', 'worker-av', 'stack-overflow'):
            assert manifest['exception_dump_present'], (case, manifest)
            dumps = [path.parent / a['path'] for a in manifest['artifacts'] if a['exception_dump']]
            assert all(d.read_bytes()[:4] == b'MDMP' for d in dumps), (case, dumps)
            if mode == 'observer-av':
                guest = manifest['guest_context']['data']
                dump = dumps[0].read_bytes()
                count, directory = struct.unpack_from('<II', dump, 8)
                exception_pc = None
                for index in range(count):
                    kind, size, rva = struct.unpack_from('<III', dump, directory + index * 12)
                    if kind == 6:
                        exception_pc = struct.unpack_from('<Q', dump, rva + 24)[0]
                assert exception_pc == guest['native_pc'], (exception_pc, guest)
                assert guest['access_operation'] == 'write' and not guest['guest_context_available'], guest
        if mode == 'fast-fail' and not manifest['exception_dump_present']:
            assert manifest['fast_fail_capture'] == 'not-observed', manifest
            assert 'WER' in manifest['dump_absent_reason'], manifest
        if mode in ('stop', 'shutdown-blocked'):
            assert manifest['raw_exit_code'] == 88, (case, manifest)
            assert manifest['first_failure']['data']['operation'] == 'verification-original-gpu-error', manifest
        if mode in ('hang', 'automatic-hang', 'shutdown-blocked'):
            assert manifest['snapshots'], (case, manifest)
            if os.name == 'nt':
                assert manifest['snapshots'][0]['present'], (case, manifest)
            if mode == 'automatic-hang':
                assert manifest['snapshots'][0]['trigger'] == 'suspected-stall', manifest
        if mode in ('normal', 'hang', 'automatic-hang'):
            assert process.returncode == 0, (case, manifest)
            assert manifest['classification'] == 'normal-exit', (case, manifest)
        if mode == 'exit':
            assert manifest['classification'] == 'interruption-undetermined', manifest
            assert manifest['raw_exit_code'] == 77, manifest
        assert not received, ('Opt-out contacted the transport endpoint', received)
        results.append({'case': case, 'manifest': str(path), 'exit_code': manifest['raw_exit_hex'],
                        'dump': manifest['exception_dump_present'], 'classification': manifest['classification']})
        print('PASS:', case, manifest['raw_exit_hex'], flush=True)
    server.shutdown()
    (output / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print(f'PASS: {len(results)} real app cases; opt-out HTTP requests: {len(received)}')


if __name__ == '__main__':
    main()
