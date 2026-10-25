"""Real Inno migrations with isolated registry, shortcuts and user files.

Set NARUTORISE_TEST_PACKAGE to the new payload, NARUTORISE_TEST_OLD_PACKAGE
to a local 0.3.0 payload, and NARUTORISE_TEST_OUTPUT to keep all test logs.
Neither the game nor the launcher is started. No personal registration is used.
"""
import hashlib
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import tomllib
import unittest
import uuid
import winreg

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / 'app/out/build/win-amd64-release'
NAME = 'Naruto - Rise of a Ninja.lnk'


class BridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(os.environ.get('NARUTORISE_TEST_OUTPUT') or tempfile.mkdtemp(prefix='narutorise-bridge-')).resolve()
        cls.root.mkdir(parents=True, exist_ok=True)
        cls.package = Path(os.environ.get('NARUTORISE_TEST_PACKAGE', ROOT / 'dist/naruto-rise-recomp-win-amd64')).resolve()
        cls.version = (cls.package / 'narutorise.version').read_text().strip()
        cls.compiler = Path(os.environ.get('NARUTORISE_TEST_ISCC', Path(os.environ['LOCALAPPDATA']) / 'Programs/Inno Setup 6/ISCC.exe'))
        cls.app_id = '{' + str(uuid.uuid4()).upper() + '}'
        cls.key = 'Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\' + cls.app_id + '_is1'
        cls.legacy = cls.key + '-Legacy'
        cls.links = cls.root / 'shortcuts'
        cls.links.mkdir(exist_ok=True)
        cls.setup = cls.compile(ROOT / 'packaging/installer.iss', cls.package, cls.version, cls.root / 'bridge')
        cls.previous = None
        old_package = Path(os.environ.get('NARUTORISE_TEST_OLD_PACKAGE', ROOT / 'dist/v0.3.0-beta/naruto-rise-recomp-win-amd64')).resolve()
        if old_package.is_dir():
            fixture = cls.root / 'old-source'
            fixture.mkdir()
            # Original installer; only test locations and AppId are isolated.
            pinned = '4355dd2'
            source = subprocess.check_output(['git', 'show', f'{pinned}:packaging/installer.iss'], cwd=ROOT).decode('utf-8')
            source = source.replace('..\\launcher\\assets', str(old_package / 'assets')).replace('..\\tools', str(ROOT / 'tools'))
            source = source.replace('{autodesktop}', str(cls.links / 'desktop')).replace('{group}', str(cls.links / 'start'))
            (fixture / 'installer.iss').write_text(source, encoding='utf-8')
            (fixture / 'locales').mkdir()
            for language in ('fr', 'de', 'es', 'it', 'ru'):
                (fixture / 'locales' / (language + '.iss')).write_bytes(subprocess.check_output(
                    ['git', 'show', f'{pinned}:packaging/locales/{language}.iss'], cwd=ROOT))
            # ISO import is never entered: retain its embedded-file slot without
            # requiring the retired helper to be built on a clean checkout.
            helper = fixture / 'narutorise_setup_helper.exe'
            shutil.copy2(old_package / 'narutorise.exe', helper)
            cls.previous = cls.compile(fixture / 'installer.iss', old_package, '0.3.0-beta', cls.root / 'old',
                                       [f'/DHelperPath={helper}'])
        print('Bridge test artifacts: ' + str(cls.root), flush=True)

    @classmethod
    def compile(cls, source, package, version, output, extra=()):
        result = subprocess.run([str(cls.compiler), '/Q', f'/O{output}', f'/DPackageDir={package}', f'/DMyAppVersion={version}',
            f'/DSetupAppId={cls.app_id.replace("{", "{{")}', f'/DLegacyUninstallKey={cls.legacy}',
            f'/DDefaultInstallDir={cls.root / "missing"}', f'/DDesktopRoot={cls.links / "desktop"}',
            f'/DStartMenuRoot={cls.links / "start"}', f'/DLegacyStartMenuRoot={cls.links / "legacy"}',
            *extra, str(source)], capture_output=True, timeout=180)
        output.mkdir(parents=True, exist_ok=True)
        (output / 'compile.log').write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError((result.stdout + result.stderr).decode(errors='replace'))
        return output / 'NarutoRiseInstaller.exe'

    def setUp(self):
        self.case = self.root / self._testMethodName
        self.case.mkdir()
        self.dest = self.case / 'instalação ação'
        self.dest.mkdir()
        self.count = 0
        self.uninstalled = False

    def tearDown(self):
        if not self.uninstalled and (self.dest / 'unins000.exe').exists():
            self.uninstall()
        for key in (self.key, self.legacy):
            try:
                winreg.DeleteKeyEx(winreg.HKEY_CURRENT_USER, key, winreg.KEY_WOW64_64KEY)
            except FileNotFoundError:
                pass

    def install(self, setup=None, mode='update', language='english', use_dir=True, icons=False):
        self.count += 1
        args = [str(setup or self.setup), '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/NOFORCECLOSEAPPLICATIONS',
                f'/MODE={mode}', f'/LANG={language}', '/TASKS=', f'/LOG={self.case / (str(self.count) + "-install.log")}']
        if not icons: args += ['/NOICONS']
        if use_dir: args += [f'/DIR={self.dest}']
        return subprocess.run(args, capture_output=True, timeout=90).returncode

    def uninstall(self):
        result = subprocess.run([str(self.dest / 'unins000.exe'), '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
                                 f'/LOG={self.case / "uninstall.log"}'], capture_output=True, timeout=90)
        self.assertEqual(result.returncode, 0)
        self.uninstalled = True

    def old_portable(self, version='0.3.0-beta'):
        for name in ('narutorise.exe', 'narutorise_launcher.exe', 'rexruntime.dll'):
            (self.dest / name).write_bytes(b'old portable executable')
        (self.dest / 'narutorise.version').write_text(version)

    def user_files(self):
        files = {
            'narutorise.toml': b'# personal settings\r\nfullscreen = false\r\nlauncher_language = "ru"\r\nuser_data_root = "my-saves"\r\n',
            'game/default.xex': b'original game XEX', 'game/data.bin': b'original assets',
            'game_root/data.bin': b'legacy assets', 'dlc/pending': b'pending STFS',
            'my-saves/saves/save.bin': b'personal save',
            'my-saves/0000000000000000/555307E5/00000002/installed.bin': b'installed DLC',
            'mods/community/texts.json': b'{"1":"My translation"}',
            'mods/pt-br/my-notes.txt': b'personal notes', 'personal.txt': b'personal file',
        }
        for name, data in files.items():
            path = self.dest / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        return files

    def preserved(self, files):
        for name, data in files.items():
            self.assertEqual((self.dest / name).read_bytes(), data, name)

    def current(self):
        for name in ('narutorise.exe', 'rexruntime.dll', 'narutorise_ai2c.dll', 'mods/pt-br/texts.json'):
            self.assertEqual(hashlib.sha256((self.dest / name).read_bytes()).digest(),
                             hashlib.sha256((self.package / name).read_bytes()).digest(), name)
        self.assertEqual((self.dest / 'narutorise.version').read_text().strip(), self.version)
        for name in ('narutorise_launcher.exe', 'narutorise_setup_helper.exe', 'extract-xiso.exe', 'install.ps1'):
            self.assertFalse((self.dest / name).exists(), name)

    def shortcut(self, path, target=None):
        command = '''$p = $env:NARUTORISE_LINK_FIXTURE | ConvertFrom-Json
$s = (New-Object -ComObject WScript.Shell).CreateShortcut($p.path)
if ($p.target) { $s.TargetPath=$p.target; $s.WorkingDirectory=(Split-Path $p.target -Parent); $s.Save() }
@{target=$s.TargetPath; cwd=$s.WorkingDirectory; args=$s.Arguments} | ConvertTo-Json -Compress'''
        path.parent.mkdir(parents=True, exist_ok=True)
        value = json.dumps({'path': str(path), 'target': str(target) if target else ''})
        return json.loads(subprocess.check_output(['pwsh', '-NoProfile', '-Command', command],
                         env=dict(os.environ, NARUTORISE_LINK_FIXTURE=value), encoding='utf-8'))

    def test_portable_update_and_repeat(self):
        self.old_portable()
        files = self.user_files()
        for _ in range(2):
            self.assertEqual(self.install(), 0)
            self.current()
            self.preserved(files)
        self.uninstall()
        self.preserved(files)

    def test_locked_launcher_rejects_before_replacing_files(self):
        self.old_portable()
        files = self.user_files()
        before = {p.relative_to(self.dest): p.read_bytes() for p in self.dest.rglob('*') if p.is_file()}
        create = ctypes.windll.kernel32.CreateFileW
        create.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                           wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        create.restype = wintypes.HANDLE
        close = ctypes.windll.kernel32.CloseHandle
        close.argtypes = [wintypes.HANDLE]
        handle = create(str(self.dest / 'narutorise_launcher.exe'), 0x80000000, 1, None, 3, 0, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
        try:
            self.assertNotEqual(self.install(), 0, 'a blocked migration must not report success')
            after = {p.relative_to(self.dest): p.read_bytes() for p in self.dest.rglob('*') if p.is_file()}
            self.assertEqual(after, before, 'preflight failure must not replace any installed file')
            with self.assertRaises(FileNotFoundError):
                winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.key)
        finally:
            close(handle)
        self.assertEqual(self.install(), 0)
        self.current()
        self.preserved(files)

    def test_registered_030_and_shortcuts(self):
        if not self.previous:
            self.skipTest('Supply NARUTORISE_TEST_OLD_PACKAGE for original Inno 0.3.0 migration')
        self.assertEqual(self.install(self.previous, mode='install'), 0)
        self.assertTrue((self.dest / 'narutorise_launcher.exe').exists())
        files = self.user_files()
        for folder in ('desktop', 'start', 'legacy'):
            self.shortcut(self.links / folder / NAME, self.dest / 'narutorise_launcher.exe')
        self.assertEqual(self.install(language='brazilianportuguese', use_dir=False, icons=True), 0)
        self.current()
        self.preserved(files)
        for folder in ('desktop', 'start', 'legacy'):
            link = self.shortcut(self.links / folder / NAME)
            self.assertEqual(Path(link['target']), self.dest / 'narutorise.exe')
            self.assertEqual(Path(link['cwd']), self.dest)
            self.assertEqual(link['args'], '')
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.key, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            self.assertEqual(winreg.QueryValueEx(key, 'DisplayVersion')[0], self.version)
        self.uninstall()
        self.preserved(files)

    def test_shortcuts_to_other_copy(self):
        self.old_portable()
        other = self.case / 'another-copy/narutorise_launcher.exe'
        for folder in ('desktop', 'legacy'):
            self.shortcut(self.links / folder / NAME, other)
        self.assertEqual(self.install(icons=True), 0)
        for folder in ('desktop', 'legacy'):
            self.assertEqual(Path(self.shortcut(self.links / folder / NAME)['target']), other)

    def test_reject_missing_invalid_mode_and_downgrade(self):
        self.assertNotEqual(self.install(), 0)
        self.assertEqual(list(self.dest.iterdir()), [])
        self.old_portable('9.0.0')
        for mode in ('update', 'install', 'invalid'):
            self.assertNotEqual(self.install(mode=mode), 0)
            self.assertEqual((self.dest / 'narutorise.exe').read_bytes(), b'old portable executable')
        self.assertTrue((self.dest / 'narutorise_launcher.exe').exists())

    def test_new_install_language(self):
        for language, code in (('english', 'en'), ('brazilianportuguese', 'pt_BR'), ('french', 'fr'),
                               ('german', 'de'), ('spanish', 'es'), ('italian', 'it'), ('russian', 'ru')):
            with self.subTest(language=language):
                self.dest = self.case / language
                self.dest.mkdir()
                self.uninstalled = False
                self.assertEqual(self.install(mode='install', language=language), 0)
                self.current()
                config = tomllib.loads((self.dest / 'narutorise.toml').read_text(encoding='utf-8-sig'))
                self.assertEqual(config['launcher_language'], code)
                self.assertEqual(config['translation_mod'], '')
                self.uninstall()

    def test_legacy_registration(self):
        self.old_portable('1.0.0-beta.1')
        files = self.user_files()
        with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, self.legacy, 0, winreg.KEY_WRITE | winreg.KEY_WOW64_64KEY) as key:
            winreg.SetValueEx(key, 'InstallLocation', 0, winreg.REG_SZ, str(self.dest))
            winreg.SetValueEx(key, 'DisplayVersion', 0, winreg.REG_SZ, '1.0.0-beta.1')
        self.assertEqual(self.install(use_dir=False), 0)
        self.current()
        self.preserved(files)
        with self.assertRaises(FileNotFoundError):
            winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.legacy)


if __name__ == '__main__':
    unittest.main()
