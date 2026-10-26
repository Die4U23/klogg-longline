"""Exercise packaging refusal paths before any toolchain or build work starts."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PWSH = shutil.which("pwsh")


@unittest.skipUnless(os.name == "nt" and PWSH, "Windows PowerShell packaging checks")
class PackagePreflightTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / ".local" / "package-tests"
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.repo = Path(self.temp.name)
        (self.repo / "tools").mkdir()
        self.script = self.repo / "tools" / "package.ps1"
        shutil.copyfile(ROOT / "tools" / "package.ps1", self.script)
        self.dist = self.repo / ".local" / "dist"
        self.dist.mkdir(parents=True)

    def tearDown(self):
        self.temp.cleanup()

    def invoke(self, run_id):
        # This isolated tree deliberately has no source, Git history or build
        # tools. Refusal must precede all of those dependencies.
        return subprocess.run(
            [PWSH, "-NoProfile", "-File", str(self.script), "-RunId", run_id, "-Zip"],
            cwd=self.repo, capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=15,
        )

    def test_unsafe_run_id_is_rejected_without_creating_outputs(self):
        for run_id in ("../outside", "..", "a/b", "C:\\outside", "x" * 65):
            with self.subTest(run_id=run_id):
                result = self.invoke(run_id)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("RunId must be", result.stdout + result.stderr)
                self.assertEqual(list(self.dist.iterdir()), [])

    def test_existing_package_directory_is_preserved(self):
        destination = self.dist / "klogg-longline-windows-x64-existing"
        destination.mkdir()
        marker = destination / "keep.txt"
        marker.write_bytes(b"previous validated package")
        result = self.invoke("existing")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Refusing to overwrite package directory", result.stdout + result.stderr)
        self.assertEqual(marker.read_bytes(), b"previous validated package")
        self.assertEqual(list(destination.iterdir()), [marker])

    def test_existing_archive_is_preserved_without_creating_directory(self):
        archive = self.dist / "klogg-longline-windows-x64-archived.zip"
        archive.write_bytes(b"previous validated archive")
        result = self.invoke("archived")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Refusing to overwrite package archive", result.stdout + result.stderr)
        self.assertEqual(archive.read_bytes(), b"previous validated archive")
        self.assertEqual(list(self.dist.iterdir()), [archive])


if __name__ == "__main__":
    unittest.main()
