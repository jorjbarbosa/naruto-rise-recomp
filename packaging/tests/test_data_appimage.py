#!/usr/bin/env python3
"""Real AppImage data/update acceptance with isolated HOME/XDG and no guest launch.

Usage: python test_data_appimage.py <version A> <version B> <new output>
Requires a Linux desktop/Vulkan and working FUSE. Also tests extract-and-run.
Only owned test process groups are stopped; save loading/clean shutdown have
separate acceptance cases. Every original and artifact stays in the new output.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    if sys.platform != 'linux':
        raise RuntimeError('Real AppImage acceptance requires Linux')
    import fcntl
    before, after, work = (Path(a).resolve() for a in sys.argv[1:])
    assert digest(before) != digest(after), 'Supply two different real AppImages'
    work.mkdir(parents=True, exist_ok=False)
    distribution = work / 'distribuição com espaços'
    distribution.mkdir()
    images = []
    for source, name in ((before, 'Naruto A.AppImage'), (after, 'Naruto B.AppImage')):
        target = distribution / name
        shutil.copy2(source, target)
        target.chmod(target.stat().st_mode | 0o100)
        images.append(target)
    home, cwd, temporary = work / 'home', work / 'other cwd', work / 'temporary'
    for path in (home, cwd, temporary): path.mkdir()
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('REX_') and k not in ('APPDIR', 'APPIMAGE', 'LD_LIBRARY_PATH')}
    env.update(HOME=str(home), TMPDIR=str(temporary),
               XDG_DATA_HOME=str(home / 'data'), XDG_CONFIG_HOME=str(home / 'config'),
               XDG_STATE_HOME=str(home / 'state'), XDG_CACHE_HOME=str(home / 'cache'))
    extracted = work / 'extract'
    extracted.mkdir()
    r = subprocess.run([str(images[0]), '--appimage-extract'], cwd=extracted, env=env,
                       capture_output=True, timeout=60)
    (work / 'extract.log').write_bytes(r.stdout + r.stderr)
    assert r.returncode == 0, 'extract version A'
    payload = extracted / 'squashfs-root/usr/lib/narutorise'
    legacy = home / 'config/narutorise/narutorise.toml'
    legacy.parent.mkdir(parents=True)
    legacy_bytes = b'task004_legacy_marker = "keep original"\n' + (payload / 'defaults/narutorise.toml').read_bytes()
    legacy.write_bytes(legacy_bytes)
    old_cache = home / 'cache/narutorise/appimage/legacy-build'
    (old_cache / 'dlc').mkdir(parents=True)
    (old_cache / '.ready').write_text('legacy wrapper fixture\n')
    (old_cache / 'narutorise.toml').symlink_to(legacy)
    (old_cache / 'narutorise').write_bytes(b'old payload cache; never executed')
    (old_cache / 'dlc/pending').write_bytes(b'old staging bytes')
    (old_cache / 'mods/community').mkdir(parents=True)
    (old_cache / 'mods/community/payload').write_bytes(b'legacy community mod')
    (old_cache / 'mods/community/.stfs_stage').mkdir()
    (old_cache / 'mods/community/.stfs_stage/payload').write_bytes(b'mod-owned resource')
    historical_log = home / 'state/narutorise/logs/old.log'
    historical_log.parent.mkdir(parents=True)
    historical_log.write_bytes(b'legacy diagnostic')
    data = home / 'data/narutorise'
    save = Path('E000000000000001/555307E5/00000001/old-save/SaveData')
    header = Path('E000000000000001/555307E5/Headers/00000001/old-save.header')
    for name, content in ((save, b'isolated old save bytes'), (header, b'isolated header/license')):
        path = data / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(content)
    results = []

    def boot(image, root, label, extract_run=False):
        previous_logs = set((root / 'logs').glob('*.log'))
        command = [str(image)]
        if extract_run: command.append('--appimage-extract-and-run')
        command += ['--setup', '--native_backend=vulkan', '--check_for_updates=false']
        with (work / (label + '-console.log')).open('wb') as console:
            process = subprocess.Popen(command, cwd=cwd, env=env, stdout=console,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            ready = False
            try:
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline:
                    logs = set((root / 'logs').glob('*.log')) - previous_logs
                    text = '\n'.join(p.read_text(encoding='utf-8', errors='replace') for p in logs)
                    if ('Setup presentation ready' in text and
                            '[narutorise] Data root: ' + str(root) in text and
                            (root / 'narutorise.toml').is_file()):
                        ready = True; break
                    if process.poll() is not None: break
                    time.sleep(.1)
                assert ready and process.poll() is None, label + ': real setup did not start; inspect console/logs'
                time.sleep(2)
                assert process.poll() is None, label + ': setup did not remain available'
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
                    try: process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL); process.wait(timeout=10)
                # The extract-and-run shell can exit before the child releases
                # its data lease. Wait on that OS lock before the next writer.
                if (root / '.narutorise.lock').exists():
                    with (root / '.narutorise.lock').open('r+b') as stream:
                        deadline = time.monotonic() + 25
                        while True:
                            try:
                                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
                                fcntl.flock(stream, fcntl.LOCK_UN)
                                break
                            except BlockingIOError:
                                assert time.monotonic() < deadline, label + ': test writer still owns lease'
                                time.sleep(.1)
            results.append(dict(label=label, command=command, setup_ready=ready,
                                exit=process.returncode, termination='owned process group after setup; clean shutdown tested separately'))
            (work / 'boots.json').write_text(json.dumps(results, indent=2), encoding='utf-8')

    boot(images[0], data, 'A-fuse')
    assert (data / 'mods/pt-br/texts.json').read_bytes() == (payload / 'defaults/mods/pt-br/texts.json').read_bytes()
    assert (data / 'mods/community/payload').read_bytes() == b'legacy community mod'
    assert (data / 'mods/community/.stfs_stage/payload').read_bytes() == b'mod-owned resource'
    custom_translation = b'{"15132":"Personal active translation"}'
    (data / 'mods/pt-br/texts.json').write_bytes(custom_translation)
    assert legacy.read_bytes() == legacy_bytes, 'source TOML changed'
    assert (data / 'user' / save).read_bytes() == b'isolated old save bytes' and not (data / save).exists()
    assert (data / 'dlc/pending').read_bytes() == b'old staging bytes'
    assert (data / 'logs/legacy/xdg/old.log').read_bytes() == b'legacy diagnostic'
    active = data / 'narutorise.toml'
    active.write_bytes(b'task004_update_marker = "preserve"\n' + active.read_bytes())
    config = active.read_bytes()
    # Cache cleanup is restricted to this test's old payload, after import.
    assert old_cache.resolve().is_relative_to(work)
    shutil.rmtree(old_cache)
    boot(images[1], data, 'B-fuse')
    boot(images[1], data, 'B-extract-run', True)
    assert active.read_bytes() == config
    assert (data / 'mods/pt-br/texts.json').read_bytes() == custom_translation
    assert (data / 'mods/community/payload').read_bytes() == b'legacy community mod'
    assert (data / 'user' / save).read_bytes() == b'isolated old save bytes'
    assert (data / 'user' / header).read_bytes() == b'isolated header/license'
    assert (data / 'dlc/pending').read_bytes() == b'old staging bytes'
    assert not list((home / 'cache/narutorise/appimage').glob('*/.ready'))
    assert not any(p.name in ('narutorise', 'librexruntime.so')
                   for p in (home / 'cache').rglob('*') if p.is_file())
    (distribution / 'data_location.json').write_text(json.dumps(
        dict(version=1, mode='portable', pending_source=str(data))), encoding='utf-8')
    portable = distribution / 'data'
    boot(images[0], portable, 'portable-A-extract-run', True)
    boot(images[1], portable, 'portable-B-fuse')
    assert (portable / 'narutorise.toml').read_bytes() == config
    assert (portable / 'mods/pt-br/texts.json').read_bytes() == custom_translation
    assert (portable / 'mods/community/payload').read_bytes() == b'legacy community mod'
    assert (portable / 'mods/community/.stfs_stage/payload').read_bytes() == b'mod-owned resource'
    assert (data / 'mods/pt-br/texts.json').read_bytes() == custom_translation
    assert (portable / 'user' / save).read_bytes() == (data / 'user' / save).read_bytes()
    assert (portable / 'dlc/pending').read_bytes() == (data / 'dlc/pending').read_bytes()
    assert 'pending_source' not in json.loads((distribution / 'data_location.json').read_text())
    assert legacy.read_bytes() == legacy_bytes and historical_log.read_bytes() == b'legacy diagnostic'
    report = dict(status='PASS', before=str(before), after=str(after), roots=[str(data), str(portable)],
                  scope='FUSE/extract-and-run setup, legacy import, two artifacts, portable, payload cache cleanup; no guest save load',
                  boots=results)
    (work / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report))

if __name__ == '__main__': main()
