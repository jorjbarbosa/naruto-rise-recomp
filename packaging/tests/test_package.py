"""Audit the actual ZIP against its staged payload; no game data is required."""
import hashlib
from pathlib import Path
import sys
import tomllib
import zipfile


def main():
    archive = Path(sys.argv[1]).resolve()
    staged = archive.with_suffix('')
    forbidden = {'narutorise_launcher.exe', 'narutorise_setup_helper.exe', 'extract-xiso.exe',
                 'install.ps1', 'NarutoRiseInstaller.exe'}
    required = {'narutorise.exe', 'narutorise_ai2c.dll', 'rexruntime.dll', 'rexgpu-xenos.dll',
                'narutorise.toml', 'narutorise.version', 'README.txt', 'mods/pt-br/mod.toml',
                'mods/pt-br/texts.json', 'licenses/README.txt', 'licenses/rmlui.txt',
                'licenses/freetype.txt', 'licenses/roboto-condensed.txt'}
    with zipfile.ZipFile(archive) as package:
        assert package.testzip() is None, 'ZIP integrity failure'
        names = {item.filename for item in package.infolist() if not item.is_dir()}
        assert required <= names, sorted(required - names)
        assert not {Path(name).name for name in names} & forbidden
        assert not any(name.startswith(('game/', 'game_root/', 'assets/')) for name in names)
        for name in names:
            assert not Path(name).is_absolute() and '..' not in Path(name).parts
            assert hashlib.sha256(package.read(name)).digest() == hashlib.sha256((staged / name).read_bytes()).digest(), name
        assert names == {path.relative_to(staged).as_posix() for path in staged.rglob('*') if path.is_file()}
        config = tomllib.loads(package.read('narutorise.toml').decode('utf-8-sig'))
        assert not config.get('custom_game_root')
        assert not config.get('user_data_root')
        assert not config.get('dlc_source_path', '').startswith('C:')
        assert package.read('narutorise.version').strip()
        print(f'PASS: {len(names)} ZIP files match staging; required modules/notices, clean defaults and obsolete-file exclusions')


if __name__ == '__main__':
    main()
