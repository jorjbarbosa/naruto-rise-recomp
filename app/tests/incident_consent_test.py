"""Use a local fake ingestion endpoint to verify revoked sessions are not replayed."""
import argparse
import gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import threading
import zlib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('product', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / 'game').mkdir()
    requests = []
    responses = [503]

    class Ingestion(BaseHTTPRequestHandler):
        def do_POST(self):
            body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
            encoding = self.headers.get('Content-Encoding', '')
            if encoding == 'gzip':
                body = gzip.decompress(body)
            elif encoding == 'deflate':
                body = zlib.decompress(body)
            requests.append(body)
            self.send_response(responses[0])
            self.end_headers()
        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Ingestion)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    allowed = {'SYSTEMROOT', 'WINDIR', 'PATH', 'COMSPEC', 'PATHEXT', 'OS',
               'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE', 'DISPLAY',
               'WAYLAND_DISPLAY', 'XDG_RUNTIME_DIR'}
    env = {key: value for key, value in os.environ.items() if key.upper() in allowed}
    env.update(SENTRY_DSN=f'http://public@127.0.0.1:{server.server_port}/1', SENTRY_ENVIRONMENT='consent-fixture')
    data = output / 'data 日本語'
    executable = args.product.resolve() / ('narutorise.exe' if os.name == 'nt' else 'narutorise')

    def run(mode):
        temporary = output / mode
        temporary.mkdir()
        process = subprocess.run([str(executable), '--data_root=' + str(data),
                                  '--cache_root=' + str(output / 'cache'),
                                  '--game_data_root=' + str(output / 'game'),
                                  '--crash_reporting=true', '--incident_test=' + mode],
                                 env={**env, 'TEMP': str(temporary), 'TMP': str(temporary)},
                                 capture_output=True, timeout=60)
        assert process.returncode == 0, (mode, process.returncode, process.stderr)

    run('revoke-pending')
    first = next((data / 'incidents').glob('session-*/manifest.json'))
    manifest = json.loads(first.read_text(encoding='utf-8'))
    assert manifest['transport']['state'] == 'revoked', manifest
    assert 'first_failure' not in manifest, manifest
    envelopes = list((first.parent / 'capture').rglob('*.envelope'))
    marker = b'Naruto Rise Sentry verification error'
    assert envelopes or any(marker in body for body in requests), 'No evidence the first session queued a report'
    boundary = len(requests)
    responses[0] = 200
    run('normal')
    assert not any(marker in body for body in requests[boundary:]), 'An older session was replayed after new authorization'
    sessions = list((data / 'incidents').glob('session-*/capture'))
    assert len(sessions) == 2 and sessions[0] != sessions[1], sessions
    server.shutdown()
    (output / 'result.json').write_text(json.dumps({'passed': True, 'sessions': 2,
        'pending_envelopes': len(envelopes), 'old_error_replayed': False}), encoding='utf-8')
    print('PASS: offline report, revocation, restart and new authorization use separate databases; old error was not replayed')


if __name__ == '__main__':
    main()
