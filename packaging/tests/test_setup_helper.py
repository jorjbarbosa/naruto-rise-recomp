"""Small synthetic XISO fixtures; no original game files are bundled or copied."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "app/out/build/win-amd64-release"
HELPER = BUILD / "narutorise_setup_helper.exe"
EXTRACTOR = BUILD / "extract-xiso.exe"


def xex(title=0x555307E5):
    header = bytearray(56)
    header[:4] = b"XEX2"
    struct.pack_into(">I", header, 20, 1)
    struct.pack_into(">II", header, 24, 0x40006, 32)
    struct.pack_into(">I", header, 44, title)
    return header


class SetupHelperTests(unittest.TestCase):
    def setUp(self):
        temp = Path(os.environ["LOCALAPPDATA"]) / "Temp/opencode"
        temp.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="narutorise-fixture-", dir=temp)
        self.root = Path(self.temp.name)
        self.cancel = self.root / "cancel"

    def tearDown(self):
        self.temp.cleanup()

    def run_helper(self, *args):
        return subprocess.run([str(HELPER), *map(str, args)], capture_output=True, timeout=30)

    def make_iso(self, title=0x555307E5):
        source = self.root / "source"
        source.mkdir()
        (source / "default.xex").write_bytes(xex(title))
        (source / "fixture.txt").write_text("synthetic fixture, not game data")
        iso = self.root / "original.iso"
        subprocess.run([str(EXTRACTOR), "-c", str(source), str(iso)],
                       check=True, capture_output=True, timeout=30)
        return iso

    def test_xex_title_id(self):
        path = self.root / "default.xex"
        path.write_bytes(xex())
        self.assertEqual(self.run_helper("--validate-xex", path).returncode, 0)
        path.write_bytes(xex(0x454108CF))
        result = self.run_helper("--validate-xex", path)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"WRONG_GAME", result.stdout)

    def test_truncated_xex(self):
        path = self.root / "default.xex"
        path.write_bytes(b"XEX2")
        self.assertNotEqual(self.run_helper("--validate-xex", path).returncode, 0)

    def test_check_and_extract(self):
        iso = self.make_iso()
        result = self.run_helper("--check", iso, self.cancel)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(b"FILES|2", result.stdout)
        self.assertIn(b"BYTES|", result.stdout)
        dest = self.root / "extracted"
        result = self.run_helper("--extract", iso, dest, self.cancel, 2)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((dest / "default.xex").read_bytes(), xex())

    def test_wrong_game_rejected_before_extraction(self):
        iso = self.make_iso(0x454108CF)
        dest = self.root / "extracted"
        result = self.run_helper("--extract", iso, dest, self.cancel, 2)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"WRONG_GAME", result.stdout)
        self.assertFalse(dest.exists())

    def test_invalid_iso(self):
        iso = self.root / "invalid.iso"
        iso.write_bytes(b"not an xbox image")
        result = self.run_helper("--check", iso, self.cancel)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"INVALID_ISO", result.stdout)

    def test_existing_destination_is_not_overwritten(self):
        iso = self.make_iso()
        dest = self.root / "existing"
        dest.mkdir()
        sentinel = dest / "keep.txt"
        sentinel.write_text("keep")
        result = self.run_helper("--extract", iso, dest, self.cancel, 2)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sentinel.read_text(), "keep")
        self.assertFalse((dest / "default.xex").exists())

    def test_cancellation(self):
        iso = self.make_iso()
        self.cancel.write_text("cancel")
        dest = self.root / "extracted"
        self.assertEqual(self.run_helper("--extract", iso, dest, self.cancel, 2).returncode, 2)
        self.assertFalse(dest.exists())

    def test_spaces_and_accents(self):
        iso = self.make_iso()
        moved = self.root / "Naruto edição original.iso"
        iso.rename(moved)
        result = self.run_helper("--check", moved, self.cancel)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_uppercase_default_xex(self):
        iso = self.make_iso()
        (self.root / 'source/default.xex').rename(self.root / 'source/DEFAULT.XEX')
        iso.unlink()
        subprocess.run([str(EXTRACTOR), '-c', str(self.root / 'source'), str(iso)],
                       check=True, capture_output=True, timeout=30)
        result = self.run_helper('--check', iso, self.cancel)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_full_disc_partition_offsets(self):
        iso = self.make_iso()
        data = iso.read_bytes()
        for offset in (0x0FD90000, 0x02080000, 0x18300000):
            with self.subTest(offset=hex(offset)):
                full = self.root / 'full.iso'
                with full.open('wb') as file:
                    file.seek(offset)
                    file.write(data)
                    # extract-xiso probes the XGD2 location before XGD3 and
                    # requires that read to succeed, as on a full-size disc.
                    file.truncate(max(offset + len(data), 0x0FD90000 + 0x10800))
                result = self.run_helper('--check', full, self.cancel)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                full.unlink()

    def test_cancellation_while_running(self):
        iso = self.make_iso()
        dest = self.root / "extracted"
        process = subprocess.Popen([str(HELPER), "--extract", str(iso), str(dest),
                                    str(self.cancel), "2"], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        self.assertIn(b"PROGRESS|", process.stdout.readline())
        self.cancel.write_text("cancel")
        process.communicate(timeout=30)
        self.assertEqual(process.returncode, 2)


if __name__ == "__main__":
    unittest.main()
