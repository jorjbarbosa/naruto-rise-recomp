"""Silent integration tests with a unique AppId and no real user shortcuts.

Set NARUTORISE_TEST_PACKAGE to the portable staging directory to test.
Only synthetic XEX/XISO files are used; the launcher/game is never started.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid
import winreg

from test_setup_helper import BUILD, EXTRACTOR, ROOT, xex


class InnoSetupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = Path(os.environ["LOCALAPPDATA"]) / "Temp/opencode"
        cls.temp = tempfile.TemporaryDirectory(prefix="narutorise-inno-tests-", dir=parent)
        cls.root = Path(cls.temp.name)
        compiler = Path(os.environ["LOCALAPPDATA"]) / "Programs/Inno Setup 6/ISCC.exe"
        package = Path(os.environ.get("NARUTORISE_TEST_PACKAGE", ROOT / "dist/naruto-rise-recomp-win-amd64"))
        cls.app_id = "NarutoRisePC-Test-" + uuid.uuid4().hex
        cls.legacy_key = 'Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\' + cls.app_id + '-Legacy'
        subprocess.run([str(compiler), f"/O{cls.root}", f"/DPackageDir={package}",
                        f"/DSetupAppId={cls.app_id}", f"/DLegacyUninstallKey={cls.legacy_key}",
                        str(ROOT / "packaging/installer.iss")],
                       check=True, capture_output=True, timeout=120)
        cls.setup = cls.root / "NarutoRiseInstaller.exe"

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.uninstalled = False
        self.dest = self.root / ("instalação teste " + uuid.uuid4().hex)
        self.dest.mkdir()
        self.case = self.root / ("fixture-" + uuid.uuid4().hex)
        self.case.mkdir()

    def tearDown(self):
        if not self.uninstalled and (self.dest / "unins000.exe").exists():
            self.uninstall()
        try:
            winreg.DeleteKeyEx(winreg.HKEY_CURRENT_USER, self.legacy_key, winreg.KEY_WOW64_64KEY)
        except FileNotFoundError:
            pass

    def install(self, iso=None, language="english"):
        args = [str(self.setup), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                "/NOICONS", "/TASKS=", f"/DIR={self.dest}", f"/LANG={language}",
                f"/LOG={self.case / ('install-' + uuid.uuid4().hex + '.log')}"]
        if iso:
            args.append(f"/GAMEISO={iso}")
        return subprocess.run(args, capture_output=True, timeout=60).returncode

    def uninstall(self):
        result = subprocess.run([str(self.dest / "unins000.exe"), "/VERYSILENT",
                                 "/SUPPRESSMSGBOXES", "/NORESTART"], timeout=60)
        self.assertEqual(result.returncode, 0)
        self.uninstalled = True

    def iso(self, title=0x555307E5):
        source = self.case / "source"
        source.mkdir()
        (source / "default.xex").write_bytes(xex(title))
        (source / "fixture.txt").write_text("synthetic data")
        iso = self.case / "edição original.iso"
        subprocess.run([str(EXTRACTOR), "-c", str(source), str(iso)],
                       check=True, capture_output=True, timeout=30)
        return iso

    def test_without_iso_and_uninstall_preservation(self):
        self.assertEqual(self.install(), 0)
        self.assertTrue((self.dest / "narutorise_launcher.exe").exists())
        self.assertTrue((self.dest / "assets/narutorise.ico").exists())
        self.assertFalse((self.dest / "narutorise_installer.exe").exists())
        self.assertFalse((self.dest / "extract-xiso.exe").exists())
        self.assertFalse((self.dest / "narutorise_setup_helper.exe").exists())
        self.assertFalse((self.dest / "game_root").exists())
        config = self.dest / "narutorise.toml"
        config.write_text('# user settings\nfullscreen = false\n')
        game = self.dest / "game/default.xex"
        game.write_bytes(xex())
        unrelated = self.dest / "personal.txt"
        unrelated.write_text("keep me")
        self.assertEqual(self.install(), 0)
        self.assertEqual(config.read_text(), '# user settings\nfullscreen = false\n')
        self.uninstall()
        self.assertFalse((self.dest / "narutorise_launcher.exe").exists())
        self.assertTrue(config.exists())
        self.assertEqual(game.read_bytes(), xex())
        self.assertEqual(unrelated.read_text(), "keep me")

    def test_iso_import_portuguese(self):
        self.assertEqual(self.install(self.iso(), "brazilianportuguese"), 0)
        self.assertEqual((self.dest / "game/default.xex").read_bytes(), xex())
        self.assertIn('launcher_language = "pt_BR"', (self.dest / "narutorise.toml").read_text(encoding='utf-8-sig'))
        self.assertFalse(list(self.dest.glob('.narutorise-import-*')))
        self.uninstall()
        self.assertTrue((self.dest / "game/default.xex").exists())

    def test_existing_data_not_overwritten(self):
        game = self.dest / "game"
        game.mkdir()
        (game / "personal.txt").write_text("keep")
        self.assertNotEqual(self.install(self.iso()), 0)
        self.assertFalse((self.dest / "narutorise_launcher.exe").exists())
        self.assertEqual((game / "personal.txt").read_text(), "keep")

    def test_empty_game_folder_can_be_imported(self):
        (self.dest / "game").mkdir()
        self.assertEqual(self.install(self.iso()), 0)
        self.assertTrue((self.dest / "game/default.xex").exists())

    def test_wrong_game_not_installed(self):
        self.assertNotEqual(self.install(self.iso(0x454108CF)), 0)
        self.assertFalse((self.dest / "narutorise_launcher.exe").exists())
        self.assertFalse((self.dest / "game/default.xex").exists())

    def test_invalid_image_not_installed(self):
        invalid = self.case / "invalid.iso"
        invalid.write_bytes(b"invalid image")
        self.assertNotEqual(self.install(invalid), 0)
        self.assertFalse((self.dest / "narutorise_launcher.exe").exists())

    def test_legacy_registration_only_same_location(self):
        with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, self.legacy_key, 0,
                                winreg.KEY_WRITE | winreg.KEY_WOW64_64KEY) as key:
            winreg.SetValueEx(key, 'InstallLocation', 0, winreg.REG_SZ, str(self.root / 'another-install'))
        self.assertEqual(self.install(), 0)
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.legacy_key, 0,
                            winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            self.assertEqual(winreg.QueryValueEx(key, 'InstallLocation')[0], str(self.root / 'another-install'))
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.legacy_key, 0,
                            winreg.KEY_WRITE | winreg.KEY_WOW64_64KEY) as key:
            winreg.SetValueEx(key, 'InstallLocation', 0, winreg.REG_SZ, str(self.dest))
        self.assertEqual(self.install(), 0)
        with self.assertRaises(FileNotFoundError):
            winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.legacy_key, 0,
                           winreg.KEY_READ | winreg.KEY_WOW64_64KEY)


if __name__ == "__main__":
    unittest.main()
