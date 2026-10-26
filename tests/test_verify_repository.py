import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from tools.verify_repository import FOLLOWUP_BASE_COMMIT, verify_repository


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class VerifyRepositoryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.sources = self.root / "upstream/klogg"
        self.sources.mkdir(parents=True)
        self.patch_dir = self.root / "patches"
        self.patch_dir.mkdir()
        self.evidence = self.root / "evidence/windows-2026-10-17/reproducibility"
        self.evidence.mkdir(parents=True)
        (self.root / "evidence/windows-2026-10-17").mkdir(exist_ok=True)

        original = b"historical source\n"
        added = b"new source\n"
        (self.sources / "CMakeLists.txt").write_bytes(original)
        (self.sources / "old.cpp").write_bytes(added)
        self.map = {
            "upstream/klogg/CMakeLists.txt": sha(original),
            "upstream/klogg/old.cpp": sha(added),
        }
        self.patch_bytes = b"stable patch data"
        (self.patch_dir / "long-line-reliability.patch").write_bytes(self.patch_bytes)
        (self.patch_dir / "long-line-reliability.json").write_text(json.dumps({
            "patch_sha256": sha(self.patch_bytes),
            "changed_files": [{
                "path": "CMakeLists.txt",
                "before_sha256": sha(b"before patch"),
                "after_sha256": self.map["upstream/klogg/CMakeLists.txt"],
            }],
        }), encoding="utf-8")
        self._write_history()

    def tearDown(self):
        self.tmp.cleanup()

    def _write_history(self):
        (self.root / "evidence/windows-2026-10-17/manifest.json").write_text(
            json.dumps({"sources": []}), encoding="utf-8")
        (self.evidence / "summary.json").write_text(
            json.dumps({"source_file_sha256": self.map}), encoding="utf-8")

    def _overlay(self, changes):
        (self.patch_dir / "followups.json").write_text(json.dumps({
            "format": 1,
            "base_commit": FOLLOWUP_BASE_COMMIT,
            "changes": changes,
        }), encoding="utf-8")

    def test_valid_overlay_updates_file_and_adds_file(self):
        before = self.map["upstream/klogg/CMakeLists.txt"]
        replacement = b"follow-up source\n"
        added = b"windows override\n"
        (self.sources / "CMakeLists.txt").write_bytes(replacement)
        (self.sources / "efsw-win32/Windows.cpp").parent.mkdir(parents=True)
        (self.sources / "efsw-win32/Windows.cpp").write_bytes(added)
        self._overlay([
            {"path": "upstream/klogg/CMakeLists.txt", "before_sha256": before, "after_sha256": sha(replacement)},
            {"path": "upstream/klogg/efsw-win32/Windows.cpp", "before_sha256": None, "after_sha256": sha(added)},
        ])
        self.assertEqual(3, verify_repository(self.root)["source_files"])

    def test_source_tampering_is_detected(self):
        (self.sources / "old.cpp").write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "最终来源映射不一致"):
            verify_repository(self.root)

    def test_new_evidence_exports_are_verified(self):
        exported = self.root / "evidence/windows-2026-10-26"
        exported.mkdir()
        payload = b"current evidence\r\n"
        record = exported / "summary.json"
        record.write_bytes(payload)
        manifest = exported / "manifest.json"
        entry = {"status": "copied", "output_path": record.relative_to(self.root).as_posix(),
                 "output_bytes": len(payload), "output_sha256": sha(payload)}
        manifest.write_text(json.dumps({"sources": [entry]}), encoding="utf-8")
        self.assertEqual(1, verify_repository(self.root)["evidence_files"])
        record.write_bytes(b"changed evidence\r\n")
        with self.assertRaisesRegex(ValueError, "导出证据完整性校验失败"):
            verify_repository(self.root)
        entry["output_path"] = "upstream/klogg/old.cpp"
        manifest.write_text(json.dumps({"sources": [entry]}), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "证据路径越界"):
            verify_repository(self.root)

    def test_undeclared_new_file_is_detected(self):
        (self.sources / "surprise.txt").write_bytes(b"surprise")
        with self.assertRaisesRegex(ValueError, "未声明新增"):
            verify_repository(self.root)

    def test_before_hash_must_match_historical_snapshot(self):
        self._overlay([{
            "path": "upstream/klogg/CMakeLists.txt",
            "before_sha256": sha(b"wrong"),
            "after_sha256": sha(b"after"),
        }])
        with self.assertRaisesRegex(ValueError, "before_sha256"):
            verify_repository(self.root)

    def test_duplicate_and_escaping_paths_are_rejected(self):
        good = {"path": "upstream/klogg/CMakeLists.txt", "before_sha256": self.map["upstream/klogg/CMakeLists.txt"], "after_sha256": sha(b"after")}
        self._overlay([good, good])
        with self.assertRaisesRegex(ValueError, "路径重复"):
            verify_repository(self.root)
        self._overlay([{"path": "upstream/klogg/../../outside", "before_sha256": None, "after_sha256": sha(b"x")}])
        with self.assertRaisesRegex(ValueError, "路径越界"):
            verify_repository(self.root)

    def test_unknown_delete_is_rejected(self):
        self._overlay([{
            "path": "upstream/klogg/missing.cpp",
            "before_sha256": None,
            "after_sha256": None,
        }])
        with self.assertRaisesRegex(ValueError, "删除未知文件"):
            verify_repository(self.root)


if __name__ == "__main__":
    unittest.main()
