"""Silent integration tests with a unique AppId and no real user shortcuts.

Set NARUTORISE_TEST_PACKAGE to the portable staging directory to test.
Only synthetic XEX/XISO files are used; the launcher/game is never started.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import tomllib
import unittest
import uuid
import winreg

from test_setup_helper import BUILD, EXTRACTOR, HELPER, ROOT, xex


class InnoSetupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = Path(os.environ["LOCALAPPDATA"]) / "Temp/opencode"
        parent.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix="narutorise-inno-tests-", dir=parent)
        cls.root = Path(cls.temp.name)
        compiler = Path(os.environ.get("NARUTORISE_TEST_ISCC",
                        Path(os.environ["LOCALAPPDATA"]) / "Programs/Inno Setup 6/ISCC.exe"))
        package = Path(os.environ.get("NARUTORISE_TEST_PACKAGE", ROOT / "dist/naruto-rise-recomp-win-amd64"))
        cls.package = package
        # Exercise the same escaped GUID AppId format as production.
        cls.app_id = "{" + str(uuid.uuid4()).upper() + "}"
        cls.legacy_key = 'Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\' + cls.app_id + '-Legacy'
        cls.current_version = os.environ.get("NARUTORISE_TEST_VERSION", "1.0.1")
        cls.uninstall_key = 'Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\' + cls.app_id + '_is1'

        def compile_setup(source, version, output):
            result = subprocess.run([str(compiler), "/Q", f"/O{output}",
                                     f"/DPackageDir={source}", f"/DHelperPath={HELPER}",
                                     f"/DMyAppVersion={version}", f"/DSetupAppId={cls.app_id.replace('{', '{{')}",
                                     f"/DLegacyUninstallKey={cls.legacy_key}",
                                     f"/DDefaultInstallDir={cls.root / 'missing-default'}",
                                     str(ROOT / "packaging/installer.iss")],
                                    capture_output=True, timeout=120)
            if result.returncode:
                raise RuntimeError((result.stdout + result.stderr).decode(errors="replace"))
            return output / "NarutoRiseInstaller.exe"

        cls.setup = compile_setup(package, cls.current_version, cls.root / "current")
        previous_package = cls.root / "previous-package"
        shutil.copytree(package, previous_package)
        for name in ("narutorise.exe", "rexruntime.dll"):
            (previous_package / name).write_bytes(b"synthetic previous-release binary")
        cls.previous_setup = compile_setup(previous_package, "1.0.0", cls.root / "previous")

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

    def install(self, iso=None, language="english", mode=None, setup=None, use_dir=True):
        args = [str(setup or self.setup), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                "/NOICONS", "/TASKS=",
                f"/LOG={self.case / ('install-' + uuid.uuid4().hex + '.log')}"]
        if use_dir:
            args.append(f"/DIR={self.dest}")
        if mode:
            args.append(f"/MODE={mode}")
        if language is not None:
            args.append(f"/LANG={language}")
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
        # Every runtime DLL staged in the package must be installed: missing
        # DLLs break loading (e.g. rexruntime.dll imports amd_fidelityfx_dx12).
        for dll in self.package.glob("*.dll"):
            self.assertTrue((self.dest / dll.name).exists(), dll.name)
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

    def test_interface_languages_preserve_native_game_language(self):
        languages = [("english", "en"), ("brazilianportuguese", "pt_BR"),
                     ("french", "fr"), ("german", "de"), ("spanish", "es"),
                     ("italian", "it"), ("russian", "ru")]
        for language, code in languages:
            with self.subTest(language=language):
                self.dest = self.root / ("language-" + uuid.uuid4().hex)
                self.dest.mkdir()
                self.uninstalled = False
                self.assertEqual(self.install(language=language), 0)
                config = tomllib.loads((self.dest / "narutorise.toml").read_text(encoding="utf-8-sig"))
                self.assertEqual(config["launcher_language"], code)
                self.assertEqual(config["user_language"], 1)
                self.uninstall()

    def test_default_install_language_is_english(self):
        self.assertEqual(self.install(language=None), 0)
        config = tomllib.loads((self.dest / "narutorise.toml").read_text(encoding="utf-8-sig"))
        self.assertEqual(config["launcher_language"], "en")

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

    def user_files(self):
        files = {
            "narutorise.toml": b'# custom settings\r\nfullscreen = false\r\nlauncher_language = "ru"\r\n',
            "game/default.xex": bytes(xex()),
            "game/custom-assets.bin": b"original game data",
            "game_root/default.xex": bytes(xex()),
            "dlc/pending-package": b"pending DLC",
            "logs/session.log": b"existing log",
            "personal.txt": b"personal file",
            "test-user-data/saves/save.bin": b"save data",
            "test-user-data/0000000000000000/555307E5/00000002/dlc.bin": b"installed DLC",
        }
        for name, content in files.items():
            path = self.dest / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        config = self.dest / "narutorise.toml"
        config.write_bytes(files["narutorise.toml"] +
                           f'user_data_root = "{(self.dest / "test-user-data").as_posix()}"\r\n'.encode())
        files["narutorise.toml"] = config.read_bytes()
        return files

    def assert_user_files(self, files):
        for name, content in files.items():
            with self.subTest(file=name):
                self.assertEqual((self.dest / name).read_bytes(), content)

    def assert_current_release(self):
        for name in ("narutorise.exe", "narutorise_launcher.exe", "rexruntime.dll", "narutorise_ai2c.dll"):
            self.assertEqual((self.dest / name).read_bytes(), (self.package / name).read_bytes(), name)
        self.assertEqual((self.dest / "narutorise.version").read_text().strip(), self.current_version)
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.uninstall_key, 0,
                            winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            self.assertEqual(winreg.QueryValueEx(key, "DisplayVersion")[0], self.current_version)
        self.assertEqual(len(list(self.dest.glob("unins*.exe"))), 1)

    def test_update_between_releases_preserves_user_data(self):
        self.assertEqual(self.install(setup=self.previous_setup, mode="install"), 0)
        self.assertNotEqual((self.dest / "narutorise.exe").read_bytes(), (self.package / "narutorise.exe").read_bytes())
        files = self.user_files()
        # Older Inno releases only recorded their version in the registry.
        (self.dest / "narutorise.version").unlink()
        self.assertEqual(self.install(mode="update", language="brazilianportuguese", use_dir=False), 0)
        self.assert_current_release()
        self.assert_user_files(files)
        self.assertEqual(self.install(mode="update"), 0)
        self.assert_user_files(files)
        self.uninstall()
        self.assert_user_files(files)
        self.assertFalse((self.dest / "narutorise.version").exists())

    def test_update_legacy_installation_detects_custom_folder(self):
        for name in ("narutorise.exe", "narutorise_launcher.exe", "narutorise_installer.exe"):
            (self.dest / name).write_bytes(b"synthetic legacy binary")
        files = self.user_files()
        with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, self.legacy_key, 0,
                                winreg.KEY_WRITE | winreg.KEY_WOW64_64KEY) as key:
            winreg.SetValueEx(key, "InstallLocation", 0, winreg.REG_SZ, str(self.dest))
            winreg.SetValueEx(key, "DisplayVersion", 0, winreg.REG_SZ, "1.0.0")
        self.assertEqual(self.install(mode="update", use_dir=False), 0)
        self.assert_current_release()
        self.assert_user_files(files)
        self.assertEqual((self.dest / "narutorise_installer.exe").read_bytes(), b"synthetic legacy binary")
        with self.assertRaises(FileNotFoundError):
            winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.legacy_key, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY)

    def test_update_portable_without_version_or_registration(self):
        for name in ("narutorise.exe", "narutorise_launcher.exe"):
            (self.dest / name).write_bytes(b"synthetic portable binary")
        files = self.user_files()
        self.assertEqual(self.install(mode="update"), 0)
        self.assert_current_release()
        self.assert_user_files(files)

    def test_update_rejects_missing_installation(self):
        self.assertNotEqual(self.install(mode="update"), 0)
        self.assertEqual(list(self.dest.iterdir()), [])
        (self.dest / "narutorise.exe").write_bytes(b"only one executable")
        self.assertNotEqual(self.install(mode="update"), 0)
        self.assertEqual(len(list(self.dest.iterdir())), 1)

    def test_update_rejects_iso_and_invalid_mode(self):
        self.assertEqual(self.install(setup=self.previous_setup), 0)
        previous = (self.dest / "narutorise.exe").read_bytes()
        self.assertNotEqual(self.install(mode="update", iso=self.iso()), 0)
        self.assertNotEqual(self.install(mode="invalid"), 0)
        self.assertEqual((self.dest / "narutorise.exe").read_bytes(), previous)
        self.assertEqual((self.dest / "narutorise.version").read_text().strip(), "1.0.0")

    def test_update_blocks_older_installer(self):
        self.assertEqual(self.install(), 0)
        files = self.user_files()
        for mode in ("update", "install"):
            with self.subTest(mode=mode):
                self.assertNotEqual(self.install(mode=mode, setup=self.previous_setup), 0)
                self.assert_current_release()
                self.assert_user_files(files)

    def test_update_compares_numeric_versions(self):
        for name in ("narutorise.exe", "narutorise_launcher.exe"):
            (self.dest / name).write_bytes(b"synthetic newer portable binary")
        (self.dest / "narutorise.version").write_text("1.0.10\n")
        self.assertNotEqual(self.install(mode="update"), 0)
        self.assertEqual((self.dest / "narutorise.exe").read_bytes(), b"synthetic newer portable binary")

    def test_existing_install_can_import_iso_in_install_mode(self):
        self.assertEqual(self.install(), 0)
        self.assertEqual(self.install(mode="install", iso=self.iso()), 0)
        self.assertEqual((self.dest / "game/default.xex").read_bytes(), xex())


if __name__ == "__main__":
    unittest.main()
