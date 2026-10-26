import tempfile
import unittest
import zipfile
from pathlib import Path

from tools.prepare_baseline import verify_snapshot


class PrepareBaselineTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.destination = self.root / "baseline"
        self.snapshot = self.destination / "klogg-test"
        self.snapshot.mkdir(parents=True)
        self.archive_path = self.root / "source.zip"
        with zipfile.ZipFile(self.archive_path, "w") as archive:
            archive.writestr("klogg-test/src/main.cpp", b"source\n")
            archive.writestr("klogg-test/.hidden-config", b"config\n")
        (self.snapshot / "src").mkdir()
        (self.snapshot / "src/main.cpp").write_bytes(b"source\n")
        (self.snapshot / ".hidden-config").write_bytes(b"config\n")

    def tearDown(self):
        self.temp.cleanup()

    def verify(self):
        with zipfile.ZipFile(self.archive_path) as archive:
            return verify_snapshot(archive, self.destination, self.snapshot)

    def test_complete_snapshot_matches_archive(self):
        self.assertEqual(2, self.verify())

    def test_changed_file_is_rejected(self):
        (self.snapshot / "src/main.cpp").write_bytes(b"changed\n")
        with self.assertRaisesRegex(RuntimeError, "Baseline differs from archive"):
            self.verify()

    def test_extra_hidden_file_is_rejected(self):
        (self.snapshot / ".unexpected").write_bytes(b"extra\n")
        with self.assertRaisesRegex(RuntimeError, "extra=.*unexpected"):
            self.verify()

    def test_missing_file_is_rejected(self):
        (self.snapshot / ".hidden-config").unlink()
        with self.assertRaisesRegex(RuntimeError, "missing=.*hidden-config"):
            self.verify()


if __name__ == "__main__":
    unittest.main()
