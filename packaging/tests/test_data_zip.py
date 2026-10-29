"""Check two real ZIP versions preserve user data when extracted over a copy.

Usage: python packaging/tests/test_data_zip.py <before.zip> <after.zip> <new output>
No game or installer is started; this tests the artifact/update filesystem contract.
"""
import hashlib
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import sys
import zipfile


def extract(archive, destination):
    with zipfile.ZipFile(archive) as package:
        for item in package.infolist():
            path = PurePosixPath(item.filename)
            assert not path.is_absolute() and not PureWindowsPath(item.filename).is_absolute()
            assert '..' not in path.parts and not item.filename.startswith(('data/', 'logs/', 'dlc/'))
        assert 'defaults/narutorise.toml' in package.namelist()
        assert 'narutorise.toml' not in package.namelist()
        package.extractall(destination)


def main():
    before, after, output = (Path(a).resolve() for a in sys.argv[1:])
    output.mkdir(parents=True, exist_ok=False)
    installation = output / 'copy with spaces'
    extract(before, installation)
    old_version = (installation / 'narutorise.version').read_text().strip()
    fixtures = {
        'data/narutorise.toml': b'launcher_language = "ru"\nunknown = "preserve"\n',
        'data/E000000000000001/555307E5/00000001/save/SaveData': b'old save bytes\x00',
        'data/E000000000000002/555307E5/00000001/save2/SaveData': b'second profile',
        'data/E000000000000001/555307E5/Headers/00000001/save.header': b'save header/license',
        'data/0000000000000000/555307E5/00000002/addon/payload': b'installed DLC',
        'data/0000000000000000/555307E5/Headers/00000002/addon.header': b'DLC header/license',
        'data/555307E5/profile/user/settings': b'profile',
        'data/achievements/555307E5.json': b'{"unlocked":1}',
        'data/dlc/pending': b'pending package', 'data/logs/001.log': b'historical log',
        'data_location.json': b'{"version":1,"mode":"portable"}',
        'narutorise.toml': b'# preserved legacy config\nuser_data_root = "my-saves"\n',
        'my-saves/user/save': b'explicit guest source',
        'dlc/legacy': b'legacy package', 'mods/community/texts.json': b'{"personal":true}',
        'mods/pt-br/texts.json': b'{"1":"legacy customization"}',
        'mods/pt-br/mod.toml': b'# legacy custom manifest',
        'data/mods/pt-br/texts.json': b'{"1":"active customization"}',
        'data/mods/pt-br/mod.toml': b'# active custom manifest',
        'data/mods/community/payload': b'community mod\x00bytes',
    }
    state = dict(version=1, destination=str(installation / 'data'), completed=[], config_base=str(installation))
    fixtures['data/.narutorise-migration/state.json'] = json.dumps(state).encode()
    for name, content in fixtures.items():
        target = installation / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
    # Repeated extraction must never restore a factory TOML over the active one.
    for _ in range(2):
        extract(after, installation)
        for name, content in fixtures.items():
            assert (installation / name).read_bytes() == content, name
    new_version = (installation / 'narutorise.version').read_text().strip()
    assert old_version != new_version, 'Supply two actual package versions'
    with zipfile.ZipFile(after) as package:
        for name in ('narutorise.exe', 'rexruntime.dll', 'defaults/narutorise.toml'):
            assert (installation / name).read_bytes() == package.read(name), name
    report = dict(status='PASS', old_version=old_version, new_version=new_version,
                  preserved_files=len(fixtures), before=str(before), after=str(after),
                  artifact_sha256={str(p): hashlib.file_digest(p.open('rb'), 'sha256').hexdigest() for p in (before, after)},
                  scope='artifact extraction only; runtime save loading requires separate acceptance')
    (output / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report))

if __name__ == '__main__':
    main()
