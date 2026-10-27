"""Exercise active mods in real ZIP/AppImage processes without input automation.

Usage: python mods_boot_test.py <zip-or-AppImage> <game ISO> <new output>
Linux requires a working desktop/Vulkan. Only owned test processes are closed.
Game assets are read in place; HOME/XDG, guest and all writable roots are fixtures.
"""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time
import tomllib
import zipfile


def snapshot(root):
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob('*') if p.is_file()}


def write_config(path, **extra):
    values = dict(renderer='native', native_backend='vulkan' if sys.platform == 'linux' else 'd3d12',
                  fullscreen=False, input_backend='sdl', check_for_updates=False,
                  skip_intro_videos=True, dlc_source_path='', log_level='debug',
                  show_startup_menu=False, translation_mod='pt-br', user_language=1,
                  task004_mods_marker='preserve')
    values.update(extra)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(k + ' = ' + json.dumps(v) for k, v in values.items()) + '\n', encoding='utf-8')


def close(process, root):
    if process.poll() is not None:
        return
    if sys.platform == 'win32':
        from ctypes import wintypes
        user32 = ctypes.windll.user32
        callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

        @callback_type
        def visit(hwnd, _):
            owner = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == process.pid:
                user32.PostMessageW(hwnd, 0x10, 0, 0)
            return True

        user32.EnumWindows(visit, 0)
    else:
        os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=25)
    except subprocess.TimeoutExpired:
        if sys.platform == 'win32':
            process.kill()
        else:
            os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=10)
        raise AssertionError('Owned test process did not close in time')
    if sys.platform == 'linux' and (root / '.narutorise.lock').exists():
        # extract-and-run's wrapper may exit before its child finishes teardown.
        # Observe the OS lease itself before starting the next test writer.
        import fcntl
        with (root / '.narutorise.lock').open('r+b') as stream:
            deadline = time.monotonic() + 25
            while True:
                try:
                    fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    fcntl.flock(stream, fcntl.LOCK_UN)
                    break
                except BlockingIOError:
                    assert time.monotonic() < deadline, 'Test writer still owns the data lease'
                    time.sleep(.1)


def main():
    artifact, game, work = (Path(arg).resolve() for arg in sys.argv[1:])
    work.mkdir(parents=True, exist_ok=False)
    distribution = work / 'distribuição 日本語 com espaços'
    distribution.mkdir()
    home, cwd, temporary, guest = (work / name for name in ('home', 'other cwd', 'temporary', 'guest'))
    for path in (home, cwd, temporary, guest):
        path.mkdir()
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('REX_') and k not in ('APPDIR', 'APPIMAGE', 'LD_LIBRARY_PATH')}
    env.update(HOME=str(home), LOCALAPPDATA=str(home / 'local'), APPDATA=str(home / 'roaming'),
               TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary),
               XDG_DATA_HOME=str(home / 'data'), XDG_CONFIG_HOME=str(home / 'config'),
               XDG_STATE_HOME=str(home / 'state'), XDG_CACHE_HOME=str(home / 'cache'))
    for path in (home / 'local', home / 'roaming'):
        path.mkdir()
    linux = sys.platform == 'linux'
    if linux:
        executable = distribution / 'Naruto 日本語.AppImage'
        shutil.copy2(artifact, executable)
        executable.chmod(0o755)
        extracted = work / 'extracted'
        extracted.mkdir()
        r = subprocess.run([str(executable), '--appimage-extract'], cwd=extracted, env=env,
                           capture_output=True, timeout=60)
        (work / 'extract.log').write_bytes(r.stdout + r.stderr)
        assert r.returncode == 0
        payload = extracted / 'squashfs-root/usr/lib/narutorise'
        data = home / 'data/narutorise'
        legacy_config = home / 'config/narutorise/narutorise.toml'
    else:
        with zipfile.ZipFile(artifact) as package:
            package.extractall(distribution)
        executable = distribution / 'narutorise.exe'
        payload = distribution
        data = distribution / 'data'
        legacy_config = distribution / 'narutorise.toml'
    seed = payload / 'defaults/mods/pt-br'
    legacy_mod = distribution / 'mods/pt-br'
    shutil.copytree(seed, legacy_mod)
    manifest = '[mod]\ntype="translation"\nname="Legacy custom PTBR"\nbase_language="en"\ntexts="texts.json"\n'
    (legacy_mod / 'mod.toml').write_text(manifest, encoding='utf-8')
    (legacy_mod / 'texts.json').write_text('{"15132":"CUSTOM MODS TEST"}', encoding='utf-8')
    community = distribution / 'mods/community/payload'
    community.parent.mkdir()
    community.write_bytes(b'community mod\x00bytes')
    (community.parent / '.stfs_stage').mkdir()
    (community.parent / '.stfs_stage/payload').write_bytes(b'mod-owned resource')
    write_config(legacy_config)
    original_mods, original_config = snapshot(distribution / 'mods'), legacy_config.read_bytes()
    results = []

    def boot(label, root, expected_name, override=None, extract_run=False):
        command = [str(executable)]
        if extract_run:
            command.append('--appimage-extract-and-run')
        command += ['--game_data_root=' + str(game), '--user_data_root=' + str(guest),
                    '--check_for_updates=false']
        current_env = dict(env)
        if override == 'cli':
            command.append('--data_root=' + str(root))
        elif override == 'env':
            current_env['REX_DATA_ROOT'] = str(root)
        before = {p: p.stat().st_mtime_ns for p in (root / 'logs').glob('*.log')}
        console_path = work / (label + '-console.log')
        text = ''
        with console_path.open('wb') as console:
            process = subprocess.Popen(command, cwd=cwd, env=current_env, stdout=console,
                                       stderr=subprocess.STDOUT, start_new_session=linux)
            try:
                deadline = time.monotonic() + 75
                while time.monotonic() < deadline:
                    logs = [p for p in (root / 'logs').glob('*.log')
                            if before.get(p) != p.stat().st_mtime_ns]
                    text = '\n'.join(p.read_text(encoding='utf-8', errors='replace') for p in logs)
                    normalized = text.replace('\\', '/')
                    if (('[translation] loaded ' + expected_name + ' (pt-br)') in normalized and
                            'from ' + (root / 'mods/pt-br').as_posix() + ';' in normalized and
                            '[translation] table rebuilt:' in normalized):
                        break
                    assert process.poll() is None, label + ': exited early; inspect ' + str(console_path)
                    time.sleep(.2)
                else:
                    raise AssertionError(label + ': translation did not rebuild a guest table')
                assert process.poll() is None, label + ': runtime exited after loading the translation'
                assert '[narutorise] Data root: ' + root.as_posix() in normalized
                config = tomllib.loads((root / 'narutorise.toml').read_text(encoding='utf-8'))
                assert config['translation_mod'] == 'pt-br' and config['user_language'] == 1
                assert config['task004_mods_marker'] == 'preserve'
            finally:
                close(process, root)
                (work / (label + '-runtime.log')).write_text(text, encoding='utf-8')
        assert process.returncode in ((0, -signal.SIGTERM) if linux else (0,)), label + ': unexpected shutdown'
        row = dict(label=label, command=command, exit=process.returncode,
                   data=str(root), translation=expected_name, guest_table_rebuilt=True,
                   input_automation=False,
                   shutdown='SIGTERM of owned process group' if linux else 'WM_CLOSE to owned windows')
        results.append(row)
        (work / 'boots.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
        print(json.dumps(row), flush=True)

    boot('legacy', data, 'Legacy custom PTBR')
    assert snapshot(data / 'mods') == original_mods
    assert legacy_config.read_bytes() == original_config
    (data / 'mods/pt-br/mod.toml').write_text(manifest.replace('Legacy custom PTBR', 'Active custom PTBR'), encoding='utf-8')
    (data / 'mods/pt-br/texts.json').write_text('{"15132":"ACTIVE CUSTOM TEST"}', encoding='utf-8')
    active_mods = snapshot(data / 'mods')
    # Repeat package replacement while active mods are customized.
    if linux:
        replacement = executable.with_name(executable.name + '.next')
        shutil.copy2(artifact, replacement)
        replacement.chmod(0o755)
        # The FUSE helper can still map the old image after the game exits.
        # Replace the path atomically instead of truncating that mapped inode.
        os.replace(replacement, executable)
    else:
        with zipfile.ZipFile(artifact) as package:
            package.extractall(distribution)
    boot('updated', data, 'Active custom PTBR', extract_run=linux)
    assert snapshot(data / 'mods') == active_mods
    descriptor = distribution / 'data_location.json'
    custom = work / 'pasta ativa 日本語 # 100%'
    descriptor.write_text(json.dumps(dict(version=1, mode='custom', path=str(custom), pending_source=str(data))), encoding='utf-8')
    boot('custom-restart', custom, 'Active custom PTBR')
    assert snapshot(custom / 'mods') == active_mods and snapshot(data / 'mods') == active_mods
    assert 'pending_source' not in json.loads(descriptor.read_text())
    # A portable activation uses the same copy protocol and external image base.
    descriptor.write_text(json.dumps(dict(version=1, mode='portable', pending_source=str(custom))), encoding='utf-8')
    portable = distribution / 'data'
    boot('portable-restart', portable, 'Active custom PTBR', extract_run=linux)
    assert snapshot(portable / 'mods') == active_mods and snapshot(custom / 'mods') == active_mods
    # Invalid saved selection and private legacy packages must be ignored by
    # general session overrides. Only the immutable curated PT-BR is seeded.
    descriptor.write_text('{invalid ignored selection', encoding='utf-8')
    for source in ('cli', 'env'):
        isolated = work / ('isolado 日本語 ' + source)
        write_config(isolated / 'narutorise.toml')
        boot(source + '-isolated', isolated, 'Português (Brasil)', override=source, extract_run=linux and source == 'env')
        assert snapshot(isolated / 'mods/pt-br') == snapshot(seed)
        assert not (isolated / 'mods/community').exists()
    assert snapshot(distribution / 'mods') == original_mods
    assert legacy_config.read_bytes() == original_config
    report = dict(status='PASS', artifact=str(artifact), sha256=hashlib.sha256(artifact.read_bytes()).hexdigest(),
                  boots=results, originals_preserved=True, input_automation=False,
                  scope='Actual translation loader and guest table rebuilding, copy/restart, update, portable and isolated overrides')
    (work / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(dict(status='PASS', boots=len(results), report=str(work / 'result.json'))))


if __name__ == '__main__':
    main()
