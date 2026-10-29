#!/usr/bin/env python3
"""Wrap an already validated Linux package without rebuilding its game binaries."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tomllib
import urllib.request

TOOL_URL = 'https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage'
TOOL_SHA256 = 'ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0'

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def fetch(url, path):
    request = urllib.request.Request(url, headers={'User-Agent': 'narutorise-packaging'})
    with urllib.request.urlopen(request, timeout=120) as response, path.open('wb') as target:
        shutil.copyfileobj(response, target)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--work-dir', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    package = args.package.resolve()
    defaults = package / 'defaults/narutorise.toml'
    if not defaults.is_file():
        raise FileNotFoundError('Build/package Linux with defaults/narutorise.toml before creating the AppImage')
    configuration = tomllib.loads(defaults.read_text(encoding='utf-8-sig'))
    for key in ('custom_game_root', 'user_data_root', 'cache_root', 'log_file', 'data_root'):
        if configuration.get(key):
            raise ValueError('Personal path in distributed defaults: ' + key)
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    if output.exists():
        raise FileExistsError(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    tools = work / 'tools'
    tools.mkdir()
    tool = tools / 'appimagetool.AppImage'
    fetch(TOOL_URL, tool)
    if sha(tool) != TOOL_SHA256:
        raise RuntimeError('appimagetool checksum mismatch')
    tool.chmod(0o755)
    request = urllib.request.Request(
        'https://api.github.com/repos/AppImage/type2-runtime/releases/tags/continuous',
        headers={'User-Agent': 'narutorise-packaging'})
    with urllib.request.urlopen(request, timeout=120) as response:
        release = json.load(response)
    asset = next(a for a in release['assets'] if a['name'] == 'runtime-x86_64')
    runtime = tools / 'runtime-x86_64'
    fetch(asset['browser_download_url'], runtime)
    digest = sha(runtime)
    if asset.get('digest') != 'sha256:' + digest:
        raise RuntimeError('AppImage runtime checksum missing or mismatched')
    provenance = {'appimagetool_url': TOOL_URL, 'appimagetool_sha256': TOOL_SHA256,
                  'runtime_release_id': release['id'], 'runtime_url': asset['browser_download_url'],
                  'runtime_sha256': digest, 'source_package': str(package)}
    (work / 'tool-identity.json').write_text(json.dumps(provenance, indent=2))
    with (work / 'tool-extract.log').open('w') as log:
        subprocess.run([str(tool), '--appimage-extract'], cwd=tools, stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    appdir = work / 'Narutorise.AppDir'
    payload = appdir / 'usr/lib/narutorise'
    payload.mkdir(parents=True)
    binaries = ['narutorise', 'librexruntime.so', 'narutorise_posix_wait_test'] + [
        'libnarutorise_ai2c' + suffix + '.so' for suffix in ['', '1', '2', '3']]
    # The native SDK is linked statically. Its separate Crashpad process must
    # survive packaging, including executable permission inside the image.
    sentry_files = []
    if (package / 'sentry-notices.txt').is_file():
        if not (package / 'crashpad_handler').is_file():
            raise FileNotFoundError('Sentry-enabled package is missing crashpad_handler')
        binaries.append('crashpad_handler')
        sentry_files.append('sentry-notices.txt')
    hashes = {}
    for name in binaries + ['narutorise.version', 'LICENSE', 'build-info.json'] + sentry_files:
        source = package / name
        shutil.copy2(source, payload / name)
        if name in binaries:
            hashes[name] = sha(source)
            if sha(payload / name) != hashes[name]:
                raise RuntimeError('Payload changed: ' + name)
            (payload / name).chmod(0o755 if not name.endswith('.so') else 0o644)
    (payload / 'defaults').mkdir()
    shutil.copy2(package / 'defaults/narutorise.toml', payload / 'defaults/narutorise.toml')
    # Curated translation resources stay immutable in the image. Never copy a
    # user's data/config/logs or locally modified mods into the distribution.
    for name in ('mod.toml', 'texts.json'):
        destination = payload / 'defaults/mods/pt-br' / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / 'mods/pt-br' / name, destination)
    if (package / 'shader_cache').is_dir():
        shutil.copytree(package / 'shader_cache', payload / 'shader_cache')
    shutil.copytree(package / 'licenses', payload / 'licenses')
    libraries = payload / 'bundled-libs'
    libraries.mkdir()
    excluded = {'libc.so.6', 'libm.so.6', 'libdl.so.2', 'libpthread.so.0', 'librt.so.1', 'libutil.so.1'}
    dependencies = {}
    env = {**os.environ, 'LD_LIBRARY_PATH': str(package)}
    for name in binaries:
        ldd = subprocess.check_output(['ldd', str(package / name)], env=env, text=True)
        if 'not found' in ldd:
            raise RuntimeError(ldd)
        for soname, source in re.findall(r'^\s*(\S+) => (/\S+) \(', ldd, re.M):
            if soname in excluded or (package / soname).is_file():
                continue
            if soname not in dependencies:
                shutil.copy2(source, libraries / soname)
                (libraries / soname).chmod(0o644)
                dependencies[soname] = {'source': source, 'sha256': sha(libraries / soname)}
    for name in ['AppRun', 'narutorise.desktop']:
        shutil.copy2(root / 'packaging/appimage' / name, appdir / name)
    (appdir / 'AppRun').chmod(0o755)
    shutil.copy2(root / 'packaging/appimage/README.txt', payload / 'README.txt')
    shutil.copy2(root / 'app/ui/images/spiral.png', appdir / 'narutorise.png')
    (appdir / '.DirIcon').symlink_to('narutorise.png')
    build_material = {'binaries': hashes, 'dependencies': dependencies,
                      'default_config': sha(payload / 'defaults/narutorise.toml'),
                      'payload_execution': 'appdir', 'data_layout': 1}
    build_id = hashlib.sha256(json.dumps(build_material, sort_keys=True).encode()).hexdigest()[:24]
    (appdir / 'build-id').write_text(build_id + '\n')
    (appdir / 'packaging-info.json').write_text(json.dumps({**provenance, **build_material,
                                                         'build_id': build_id}, indent=2))
    subprocess.run(['sh', '-n', str(appdir / 'AppRun')], check=True)
    executable = tools / 'squashfs-root/AppRun'
    env = {**os.environ, 'ARCH': 'x86_64'}
    with (work / 'build.log').open('w') as log:
        subprocess.run([str(executable), '--runtime-file', str(runtime), '--no-appstream',
                        str(appdir), str(output)], env=env, stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    output.chmod(0o755)
    digest = sha(output)
    output.with_suffix(output.suffix + '.sha256').write_text(digest + '  ' + output.name + '\n')
    result = {'appimage': str(output), 'bytes': output.stat().st_size, 'sha256': digest,
              'build_id': build_id, 'unchanged_game_binaries': hashes,
              'bundled_dependencies': dependencies}
    (work / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result))

if __name__ == '__main__':
    main()
