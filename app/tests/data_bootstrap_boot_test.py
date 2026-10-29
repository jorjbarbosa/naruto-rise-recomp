"""Exercise the real Windows bootstrap using an isolated product and data roots.

Usage: python -I app/tests/data_bootstrap_boot_test.py <product> <game root/iso> <new output> [--xenos-only | --errors-only]
--errors-only checks rejection before presentation/runtime; it needs no GPU.
The game assets are read in place. No personal data folder is discovered/imported:
session overrides isolate most cases; the legacy case has a fixture guest root
and a distribution descriptor, so it never consults personal storage.
"""
import ctypes
from ctypes import wintypes
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import tomllib

spec = importlib.util.spec_from_file_location('setup_boot', Path(__file__).with_name('setup_boot_test.py'))
win = importlib.util.module_from_spec(spec)
spec.loader.exec_module(win)


def main():
    source, game, output = (Path(value).resolve() for value in sys.argv[1:4])
    xenos_only = sys.argv[4:] == ['--xenos-only']
    errors_only = sys.argv[4:] == ['--errors-only']
    assert len(sys.argv) == 4 or xenos_only or errors_only, 'unexpected arguments'
    output.mkdir(parents=True, exist_ok=False)
    product = output / 'product'
    product.mkdir()
    for item in source.iterdir():
        if item.name == 'narutorise.exe' or item.suffix.lower() == '.dll':
            shutil.copy2(item, product / item.name)
    shutil.copytree(source / 'defaults', product / 'defaults')
    # Exercise the distributed shader seeds with the default Unicode cache path.
    # A minimal product without these files misses post-bootstrap path failures.
    if (source / 'shader_cache').is_dir():
        (product / 'shader_cache').mkdir()
        for item in (source / 'shader_cache').iterdir():
            if item.is_file():
                shutil.copy2(item, product / 'shader_cache' / item.name)
    cwd = output / 'other cwd 日本語'
    cwd.mkdir()
    temporary = output / 'temporary'
    temporary.mkdir()
    env = {key: value for key, value in os.environ.items() if not key.startswith('REX_')}
    env.update(TEMP=str(temporary), TMP=str(temporary))
    descriptor = product / 'data_location.json'
    # Explicit data_root must ignore even a malformed persisted selection.
    descriptor.write_text('{invalid descriptor', encoding='utf-8')
    results = []

    def record(name, **values):
        row = dict(case=name, passed=True, **values)
        results.append(row)
        (output / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(row), flush=True)

    def configure(data, api='d3d12', guest='', show_menu=True, renderer='native'):
        data.mkdir(parents=True, exist_ok=True)
        config = dict(renderer=renderer, native_backend=api, fullscreen=False, input_backend='sdl',
                      check_for_updates=False, skip_intro_videos=True, dlc_source_path='', log_level='debug',
                      user_data_root=guest, show_startup_menu=show_menu, task004_unknown='preserve me')
        if renderer == 'xenos':
            config['gpu_plugin'] = 'xenos'
        (data / 'narutorise.toml').write_text('\n'.join(f'{key} = {json.dumps(value, ensure_ascii=False)}'
                                                      for key, value in config.items()), encoding='utf-8')

    def close(process):
        if process.poll() is None:
            hwnd = win.window(process.pid)
            if hwnd:
                win.user32.PostMessageW(hwnd, 0x10, 0, 0)
            try:
                process.wait(timeout=25)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise

    def failure(name, args, needle, data=None):
        before = set((data / 'logs').glob('*.log')) if data else set()
        console_path = output / (name + '-console.log')
        with console_path.open('wb') as console:
            process = subprocess.Popen([str(product / 'narutorise.exe'), *args], cwd=cwd,
                                       env=env, stdout=console, stderr=subprocess.STDOUT)
            try:
                win.wait_for(lambda: needle in console_path.read_text(encoding='utf-8', errors='replace'), process)
                # Dismiss only the error dialog owned by this test process.
                hwnd = win.wait_for(lambda: win.window(process.pid), process)
                win.user32.PostMessageW(hwnd, 0x10, 0, 0)  # WM_CLOSE
                process.wait(timeout=20)
                assert process.returncode != 0, 'bootstrap error must fail initialization'
                if data:
                    assert set((data / 'logs').glob('*.log')) == before, 'failed bootstrap wrote normal data logs'
                logs = list((temporary / 'narutorise/bootstrap').glob(f'narutorise_bootstrap_{process.pid}_*.log'))
                assert logs, 'bootstrap failure needs a diagnostic file'
                text = logs[-1].read_text(encoding='utf-8', errors='replace')
                assert needle in text and 'Native renderer backend:' not in text
                assert not any(marker in text for marker in (
                    'Setup screen:', 'Setup presentation ready', 'Initializing shader storage for title',
                    '[narutorise] Data root:'))
                shutil.copy2(logs[-1], output / (name + '-diagnostic.log'))
                record(name, exit=process.returncode)
            finally:
                close(process)

    def boot(name, api='d3d12', direct=False, default_config=False, explicit_log=False,
             extra_env=None, legacy=False, check_lock=False, environment_root=False, guest_overrides=False,
             renderer='native'):
        data = product / 'data' if legacy else output / ('data 日本語-' + name)
        if default_config:
            data.mkdir()
        else:
            configure(data, api, guest='toml guest' if guest_overrides else '', show_menu=not direct, renderer=renderer)
        expected_guest = data
        expected_cache = data / 'cache'
        if guest_overrides:
            expected_guest = output / 'CLI guest 日本語'
            expected_guest.mkdir()
            expected_cache = output / 'environment cache 日本語'
        legacy_before = None
        if legacy:
            descriptor.write_text(json.dumps(dict(version=1, mode='default')), encoding='utf-8')
            guest = output / 'legacy guest 日本語'
            guest.mkdir()
            configure(output / 'legacy config', api, guest=str(Path('..') / guest.name))
            legacy_before = (output / 'legacy config/narutorise.toml').read_bytes()
            (product / 'narutorise.toml').write_bytes(legacy_before)
            # Remove only this fixture's just-created active file, so import
            # actually runs. All other roots remain outside the product tree.
            (data / 'narutorise.toml').unlink()
            (product / 'logs').mkdir()
            (product / 'logs/narutorise_900.log').write_text('historical log', encoding='utf-8')
            expected_guest = guest
        sentinel = expected_guest / 'keep-save.bin'
        sentinel.write_bytes(b'existing isolated save')
        args = [] if legacy or environment_root else ['--data_root=' + str(data)]
        if guest_overrides:
            args += ['--user_data_root=' + str(expected_guest)]
        args += ['--game_data_root=' + str(game), '--check_for_updates=false']
        if not direct:
            args += ['--setup']
        log_path = cwd / 'chosen 日本語.log' if explicit_log else None
        if explicit_log:
            args += ['--log_file=' + log_path.name]
        current_env = dict(env, **(extra_env or {}))
        if environment_root:
            current_env['REX_DATA_ROOT'] = str(data)
        if guest_overrides:
            current_env.update(REX_USER_DATA_ROOT=str(output / 'ignored env guest'), REX_CACHE_ROOT=str(expected_cache))
        with (output / (name + '-console.log')).open('wb') as console:
            process = subprocess.Popen([str(product / 'narutorise.exe'), *args], cwd=cwd,
                                       env=current_env, stdout=console, stderr=subprocess.STDOUT)
            try:
                def text():
                    nonlocal log_path
                    if not explicit_log:
                        logs = list((data / 'logs').glob('*.log'))
                        if logs:
                            log_path = max(logs, key=lambda path: path.stat().st_mtime_ns)
                    return log_path.read_text(encoding='utf-8', errors='replace') if log_path and log_path.exists() else ''
                ready = ('Initializing shader storage for title' if renderer == 'xenos' else
                         'Native renderer backend:' if direct else 'Setup presentation ready')
                win.wait_for(lambda: ready in text(), process, timeout=45)
                assert process.poll() is None, 'bootstrap did not remain available'
                normalized = text().replace('\\', '/')
                assert '[narutorise] Data root: ' + data.as_posix() in normalized
                assert '[narutorise] Active config: ' + (data / 'narutorise.toml').as_posix() in normalized
                if not default_config:
                    assert 'User data:      ' + expected_guest.as_posix() in normalized
                if guest_overrides:
                    assert 'Cache root:     ' + expected_cache.as_posix() in normalized
                    assert not (output / 'ignored env guest').exists()
                assert sentinel.read_bytes() == b'existing isolated save'
                if direct:
                    assert 'Setup screen:' not in text()
                    time.sleep(1)
                if check_lock:
                    failure('second-instance', ['--data_root=' + str(data)], 'locked', data)
                    assert process.poll() is None, 'original writer must remain running'
                hwnd = win.wait_for(lambda: win.window(process.pid), process)
                win.user32.PostMessageW(hwnd, 0x10, 0, 0)
                process.wait(timeout=25)
                assert process.returncode == 0
                config = tomllib.loads((data / 'narutorise.toml').read_text(encoding='utf-8'))
                if not default_config:
                    assert config['task004_unknown'] == 'preserve me'
                if legacy:
                    assert (product / 'narutorise.toml').read_bytes() == legacy_before, 'legacy source TOML changed'
                    assert (data / 'logs/legacy/install/narutorise_900.log').read_text() == 'historical log'
                    assert (product / 'logs/narutorise_900.log').read_text() == 'historical log'
                else:
                    assert not (product / 'logs').exists(), 'normal logs appeared alongside executable'
                    assert not (product / 'narutorise.toml').exists(), 'preferences appeared alongside executable'
                if explicit_log:
                    assert not list((data / 'logs').glob('*.log')), 'log_file must override sequential logs'
                shutil.copy2(log_path, output / (name + '.log'))
                record(name, exit=process.returncode, data=str(data), guest=str(expected_guest))
            finally:
                close(process)

    if xenos_only:
        boot('direct-xenos', direct=True, renderer='xenos')
        print('PASS: isolated Xenos bootstrap', flush=True)
        return

    if not errors_only:
        boot('defaults', default_config=True)
        boot('setup-d3d12', check_lock=True)
        boot('setup-vulkan', api='vulkan')
        boot('direct-d3d12', direct=True)
        boot('direct-vulkan', api='vulkan', direct=True)
        boot('log-override', explicit_log=True)
        ignored = output / 'ignored environment root'
        boot('cli-over-env', extra_env=dict(REX_DATA_ROOT=str(ignored)))
        assert not ignored.exists(), 'CLI should not create the lower-priority environment root'
        boot('environment-root', environment_root=True)
        boot('guest-cache-overrides', guest_overrides=True)

    if errors_only:
        # Hold the same exclusive Windows lease as PrepareData, independently
        # of presentation. Only this new fixture is locked by this process.
        locked = output / 'locked data 日本語'
        configure(locked)
        config_before = (locked / 'narutorise.toml').read_bytes()
        kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                        ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel32.CreateFileW.restype = wintypes.HANDLE
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL
        handle = kernel32.CreateFileW(str(locked / '.narutorise.lock'), 0xC0000000, 0, None, 4, 0x80, None)
        assert handle != ctypes.c_void_p(-1).value, ctypes.WinError(ctypes.get_last_error())
        try:
            failure('second-instance', ['--data_root=' + str(locked)], 'locked', locked)
            assert (locked / 'narutorise.toml').read_bytes() == config_before
            assert not (locked / '.narutorise-migration').exists(), 'locked root was modified'
        finally:
            assert kernel32.CloseHandle(handle)

    bad = output / 'invalid active TOML'
    bad.mkdir()
    (bad / 'narutorise.toml').write_text('invalid = [', encoding='utf-8')
    failure('invalid-toml', ['--data_root=' + str(bad)], 'Invalid TOML', bad)
    assert (bad / 'narutorise.toml').read_text() == 'invalid = ['
    failure('invalid-descriptor', [], 'Invalid data descriptor')
    assert descriptor.read_text(encoding='utf-8') == '{invalid descriptor'
    occupied = output / 'occupied data root'
    occupied.write_bytes(b'preserve occupied file')
    failure('occupied-root', ['--data_root=' + str(occupied)], 'Data preparation failed')
    assert occupied.read_bytes() == b'preserve occupied file'

    incomplete = output / 'incomplete migration'
    (incomplete / '.narutorise-migration').mkdir(parents=True)
    journal = dict(version=1, destination=str(incomplete), completed=[], transaction=dict(isolated=False))
    (incomplete / '.narutorise-migration/state.json').write_text(json.dumps(journal), encoding='utf-8')
    journal_before = (incomplete / '.narutorise-migration/state.json').read_bytes()
    failure('incomplete-migration', ['--data_root=' + str(incomplete)], 'Incomplete legacy migration', incomplete)
    assert (incomplete / '.narutorise-migration/state.json').read_bytes() == journal_before
    assert not (incomplete / 'narutorise.toml').exists(), 'incomplete migration created defaults'
    if errors_only:
        assert len(results) == 5
        assert not (product / 'logs').exists() and not (product / 'narutorise.toml').exists()
        print(f'PASS: {len(results)} isolated bootstrap rejection cases; no presentation/guest acceptance', flush=True)
    else:
        boot('legacy-import', legacy=True)
        print(f'PASS: {len(results)} isolated real-app bootstrap cases', flush=True)


if __name__ == '__main__':
    main()
