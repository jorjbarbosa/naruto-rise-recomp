"""Check legacy TOML user roots, Unicode and override precedence in the real app.

Usage: python -I app/tests/user_data_boot_test.py <product> <game.iso> <new output>
All configured roots are private test folders. The first cases stop in setup.
"""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

spec = importlib.util.spec_from_file_location('setup_boot', Path(__file__).with_name('setup_boot_test.py'))
win = importlib.util.module_from_spec(spec)
spec.loader.exec_module(win)


def main():
    source, iso, output = (Path(arg).resolve() for arg in sys.argv[1:])
    output.mkdir(parents=True, exist_ok=False)
    product = Path(tempfile.mkdtemp(prefix='narutorise-user-root-'))
    for item in source.iterdir():
        if item.name == 'narutorise.exe' or item.suffix.lower() == '.dll':
            shutil.copy2(item, product / item.name)
    results = []
    for name in ('toml-relative-unicode', 'environment', 'command-line', 'cli-over-env',
                 'relative-cli', 'trailing-cli', 'direct', 'cache-override'):
        case = product / name
        case.mkdir()
        configured = case / 'saves-ação'
        env_root, cli_root = case / 'env-ação', case / 'cli-ação'
        expected = cli_root if name in ('command-line', 'cli-over-env', 'relative-cli', 'trailing-cli') else env_root if name == 'environment' else configured
        expected_cache = case / 'cache-ação' if name == 'cache-override' else expected / 'cache'
        expected.mkdir()
        sentinel = expected / 'keep-save.bin'
        sentinel.write_bytes(b'existing save')
        config = {'renderer': 'native', 'native_backend': 'd3d12', 'fullscreen': False,
                  'custom_game_root': str(iso), 'user_data_root': str(configured.relative_to(product)),
                  'check_for_updates': False, 'log_level': 'debug', 'skip_intro_videos': True}
        (product / 'narutorise.toml').write_text('\n'.join(f'{k} = {json.dumps(v, ensure_ascii=False)}' for k, v in config.items()), encoding='utf-8')
        env = {k: v for k, v in os.environ.items() if k not in ('REX_USER_DATA_ROOT', 'REX_CACHE_ROOT')}
        if name in ('environment', 'cli-over-env'): env['REX_USER_DATA_ROOT'] = str(env_root)
        args = [] if name == 'direct' else ['--setup']
        if name in ('command-line', 'cli-over-env'): args += ['--user_data_root=' + str(cli_root)]
        if name == 'relative-cli': args += ['--user_data_root=' + cli_root.name]
        if name == 'trailing-cli': args += ['--user_data_root=' + str(cli_root) + '\\']
        if name == 'direct': args += ['--game_data_root=' + str(iso)]
        if name == 'cache-override': args += ['--cache_root=' + str(expected_cache)]
        before = set((product / 'logs').glob('*.log'))
        process = subprocess.Popen([str(product / 'narutorise.exe'), *args], cwd=case, env=env)
        log = None
        try:
            hwnd = win.wait_for(lambda: win.window(process.pid), process)
            def text():
                nonlocal log
                logs = set((product / 'logs').glob('*.log')) - before
                if logs: log = max(logs, key=lambda p: p.stat().st_mtime)
                if not log: return ''
                data = log.read_bytes()
                try: return data.decode('utf-8')
                except UnicodeDecodeError: return data.decode('mbcs')
            if name == 'direct':
                win.wait_for(lambda: 'Native renderer backend:' in text(), process)
                time.sleep(1)
                assert 'Setup screen:' not in text()
            else:
                win.wait_for(lambda: 'Setup screen: requested' in text(), process)
                assert (expected / 'launch.marker').is_file(), 'marker must use the effective root'
            logged = text().replace('\\', '/')
            assert 'User data:      ' + expected.as_posix() in logged, logged[-2000:]
            assert 'Cache root:     ' + expected_cache.as_posix() in logged, logged[-2000:]
            win.user32.PostMessageW(hwnd, 0x10, 0, 0)
            process.wait(timeout=25)
            assert process.returncode == 0
            assert not (expected / 'launch.marker').exists()
            assert sentinel.read_bytes() == b'existing save'
            shutil.copy2(log, output / (name + '.log'))
            result = {'case': name, 'passed': True, 'exit': process.returncode, 'root': str(expected), 'cache': str(expected_cache)}
            results.append(result)
            print(json.dumps(result), flush=True)
            (output / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
        finally:
            if process.poll() is None:
                hwnd = win.window(process.pid)
                if hwnd: win.user32.PostMessageW(hwnd, 0x10, 0, 0)
                try: process.wait(timeout=25)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    print('PASS: TOML roots, Unicode, CLI/environment precedence and cache', flush=True)


if __name__ == '__main__':
    main()
