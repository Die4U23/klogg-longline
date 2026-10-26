import hashlib
import json
import os
import shutil
import tempfile
import unittest
from unittest import mock
from pathlib import Path

from tools import export_patch


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("git"), "Git is required for actual patch application")
class ExportPatchTests(unittest.TestCase):
    def test_export_applies_and_reconstructs_mixed_line_endings_exactly(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base = root / "base"
            candidate = root / "candidate"
            scratch = root / "scratch"
            output = root / "out" / "current-source.patch"
            base.mkdir()
            candidate.mkdir()

            original = {
                "modified-crlf.txt": b"before\r\nkeep\r\n",
                "mixed.txt": b"one\r\ntwo\nthree\r\n",
                "no-newline.txt": b"old final line",
                "deleted.txt": b"remove me\r\n",
                "deleted-empty.txt": b"",
                ".gitignore": b"ignored.txt\n",
                "ignored.txt": b"old ignored source\r\n",
            }
            updated = {
                "modified-crlf.txt": b"after\r\nkeep\r\n",
                "added-crlf.txt": b"new file\r\nsecond line\r\n",
                "mixed.txt": b"one\r\ntwo changed\nthree\r\n",
                "no-newline.txt": b"new final line",
                ".gitignore": b"ignored.txt\n",
                "ignored.txt": b"updated ignored source\r\n",
                "added-empty.txt": b"",
            }
            for name, data in original.items():
                (base / name).write_bytes(data)
            for name, data in updated.items():
                (candidate / name).write_bytes(data)

            user_config = root / "caller.gitconfig"
            config_bytes = b"[core]\n\tautocrlf = true\n"
            user_config.write_bytes(config_bytes)
            with mock.patch.dict(os.environ, {
                "GIT_CONFIG": str(user_config),
                "GIT_CONFIG_PARAMETERS": "'core.autocrlf=true'",
            }):
                report = export_patch.export_patch(base, candidate, output, scratch)
            self.assertEqual(config_bytes, user_config.read_bytes())

            self.assertEqual("passed", report["reconstruction_check"])
            self.assertEqual(len(updated), report["reconstruction_verified_files"])
            self.assertEqual(hashlib.sha256(output.read_bytes()).hexdigest(), report["patch_sha256"])
            self.assertEqual(json.loads(output.with_suffix(".json").read_text(encoding="utf-8")), report)
            self.assertEqual(
                {name for name in original.keys() | updated.keys() if original.get(name) != updated.get(name)},
                {entry["path"] for entry in report["changed_files"]},
            )
            after_hashes = {entry["path"]: entry["after_sha256"] for entry in report["changed_files"]}
            for name, data in updated.items():
                if name not in after_hashes:
                    continue
                self.assertEqual(hashlib.sha256(data).hexdigest(), after_hashes[name])
            self.assertIsNone(after_hashes["deleted.txt"])
            self.assertIsNone(after_hashes["deleted-empty.txt"])
            self.assertEqual(hashlib.sha256(b"").hexdigest(), after_hashes["added-empty.txt"])
            self.assertIn(b"\r\n", output.read_bytes())
            self.assertEqual([], list(scratch.iterdir()))

    def test_existing_patch_or_report_is_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base = root / "base"
            candidate = root / "candidate"
            scratch = root / "scratch"
            output = root / "current-source.patch"
            base.mkdir()
            candidate.mkdir()
            (base / "file.txt").write_text("old\n", encoding="utf-8")
            (candidate / "file.txt").write_text("new\n", encoding="utf-8")

            output.write_bytes(b"historical patch")
            with self.assertRaisesRegex(FileExistsError, "Refusing to overwrite"):
                export_patch.export_patch(base, candidate, output, scratch)
            self.assertEqual(b"historical patch", output.read_bytes())
            self.assertFalse(output.with_suffix(".json").exists())

            output.unlink()
            report = output.with_suffix(".json")
            report.write_bytes(b"historical report")
            with self.assertRaisesRegex(FileExistsError, "Refusing to overwrite"):
                export_patch.export_patch(base, candidate, output, scratch)
            self.assertFalse(output.exists())
            self.assertEqual(b"historical report", report.read_bytes())

            ambiguous = root / "ambiguous.json"
            with self.assertRaisesRegex(ValueError, "must use different paths"):
                export_patch.export_patch(base, candidate, ambiguous, scratch)
            self.assertFalse(ambiguous.exists())


if __name__ == "__main__":
    unittest.main()
