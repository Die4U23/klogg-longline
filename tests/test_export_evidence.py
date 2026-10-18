import contextlib
import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from tools import export_evidence


class ExportEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.repo = Path(self.temp.name) / "repo"
        self.repo.mkdir()
        self.evidence = self.repo / ".local" / "evidence"
        self.source = self.evidence / "custom"
        self.source.mkdir(parents=True)

    def tearDown(self):
        self.temp.cleanup()

    def test_no_defaults_exports_only_added_label_and_records_hashes(self):
        summary = b'{"status":"ok"}\n'
        jsonl = b'{"row":1}\n{"row":2}\n'
        large_json = b'{"large":"' + b"x" * 256 + b'"}\n'
        text = b"log-line\n" * 300
        version = b"compiler version 1\n"
        (self.source / "summary.json").write_bytes(summary)
        (self.source / "rows.jsonl").write_bytes(jsonl)
        (self.source / "results.json").write_bytes(large_json)
        (self.source / "stdout.txt").write_bytes(text)
        (self.source / "compiler-version.txt").write_bytes(version)
        (self.source / "samples.jsonl").write_text('{"sample":1}\n', encoding="utf-8")
        (self.source / "ignored.bin").write_bytes(b"ignore")

        output = export_evidence.export(
            self.repo, Path("exported"), {"selected": ".local/evidence/custom"},
            text_limit=512, json_limit=128, no_defaults=True,
        )
        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        entries = {Path(entry.get("source_path", "")).name: entry for entry in manifest["sources"]}
        self.assertEqual({entry["label"] for entry in manifest["sources"]}, {"selected"})

        for name, raw in (("summary.json", summary), ("rows.jsonl", jsonl), ("compiler-version.txt", version)):
            entry = entries[name]
            copied = output / "selected" / name
            self.assertEqual(entry["status"], "copied")
            self.assertEqual(entry["source_bytes"], len(raw))
            self.assertEqual(entry["source_sha256"], hashlib.sha256(raw).hexdigest())
            self.assertEqual(entry["output_bytes"], len(raw))
            self.assertEqual(entry["output_sha256"], hashlib.sha256(copied.read_bytes()).hexdigest())
            self.assertEqual(copied.read_bytes(), raw)

        skipped = entries["results.json"]
        self.assertEqual(skipped["status"], "skipped_too_large")
        self.assertEqual(skipped["source_bytes"], len(large_json))
        self.assertEqual(skipped["source_sha256"], hashlib.sha256(large_json).hexdigest())
        self.assertFalse((output / "selected" / "results.json").exists())

        truncated = entries["stdout.txt"]
        copied_text = (output / "selected" / "stdout.txt").read_bytes()
        self.assertEqual(truncated["status"], "copied")
        self.assertTrue(truncated["truncated"])
        self.assertEqual(truncated["source_bytes"], len(text))
        self.assertEqual(truncated["source_sha256"], hashlib.sha256(text).hexdigest())
        self.assertLessEqual(truncated["output_bytes"], 512)
        self.assertEqual(truncated["output_bytes"], len(copied_text))
        self.assertEqual(truncated["output_sha256"], hashlib.sha256(copied_text).hexdigest())
        self.assertIn(hashlib.sha256(text).hexdigest().encode("ascii"), copied_text)
        self.assertNotIn("samples.jsonl", entries)
        self.assertNotIn("ignored.bin", entries)

    def test_cli_no_defaults_option_exports_added_source(self):
        (self.source / "summary.json").write_text('{"ok":true}\n', encoding="utf-8")
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            status = export_evidence.main([
                "--repo-root", str(self.repo), "--output", "cli-export",
                "--no-defaults", "--add", "only=.local/evidence/custom",
            ])
        self.assertEqual(status, 0)
        manifest = json.loads((self.repo / "cli-export" / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual([entry["label"] for entry in manifest["sources"]], ["only"])
        self.assertIn("Manifest:", stdout.getvalue())

    def test_default_sources_remain_included_without_no_defaults(self):
        for source in export_evidence.DEFAULT_SOURCES.values():
            (self.repo / source).mkdir(parents=True, exist_ok=True)
        (self.source / "summary.json").write_text('{"ok":true}\n', encoding="utf-8")
        output = export_evidence.export(
            self.repo, Path("default-export"), {"extra": ".local/evidence/custom"},
            text_limit=512, json_limit=128,
        )
        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        labels = {entry["label"] for entry in manifest["sources"]}
        self.assertEqual(labels, set(export_evidence.DEFAULT_SOURCES) | {"extra"})


if __name__ == "__main__":
    unittest.main()
