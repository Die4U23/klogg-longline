import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

from tools import run_probe


# On Windows, a venv's sys.executable can be a launcher that starts a second
# interpreter process. The probe samples its direct child, so tests must launch
# the base interpreter itself when asserting that child's private-byte limit.
TEST_PYTHON = getattr(sys, "_base_executable", None) or sys.executable


class RunProbeTests(unittest.TestCase):
    def execute(self, root, code, *, timeout=3, max_private_mib=None, input_path=None):
        evidence = root / "evidence"
        args = [
            "--exe", TEST_PYTHON,
            "--output", str(evidence),
            "--timeout", str(timeout),
        ]
        if max_private_mib is not None:
            args += ["--max-private-mib", str(max_private_mib)]
        if input_path is not None:
            args += ["--input", str(input_path)]
        options, child_args = run_probe.parse_args(args + ["--", "-c", code])
        with contextlib.redirect_stdout(io.StringIO()):
            status = run_probe.run(options, child_args, run_probe.require_psutil())
        summary = json.loads((evidence / "summary.json").read_text(encoding="utf-8"))
        stdout = (evidence / "stdout.txt").read_bytes()
        return status, summary, stdout

    def test_private_limit_terminates_launched_child_and_reports_measurement(self):
        psutil = run_probe.require_psutil()
        if not hasattr(psutil.Process().memory_info(), "private"):
            self.skipTest("psutil private-byte sampling is unavailable on this platform")
        # Keep a grandchild alive with inherited stdout/stderr handles. This
        # reproduces Windows temporary-directory cleanup failures when only
        # the direct Python child is terminated.
        code = (
            "import os,subprocess,sys,time; "
                    "grandchild=subprocess.Popen([sys.executable,'-c','import time;time.sleep(30)']); "
            "os.write(1,str(grandchild.pid).encode()); "
            "data=bytearray(40*1024*1024); "
            "data[::4096]=b'X'*(len(data)//4096); time.sleep(30)"
        )
        with tempfile.TemporaryDirectory() as temp:
            status, summary, stdout = self.execute(Path(temp), code, max_private_mib=20)
        limit = summary["resource_limit"]
        self.assertEqual(status, 125)
        self.assertEqual(summary["runner_status"], "resource_limit")
        self.assertTrue(limit["exceeded"])
        self.assertEqual(limit["max_private_mib"], 20)
        self.assertEqual(limit["threshold_bytes"], 20 * 1024 * 1024)
        self.assertGreater(limit["measured_private_bytes_at_termination"], limit["threshold_bytes"])
        self.assertTrue(limit["runner_terminated_child"])
        self.assertFalse(summary["timed_out"])
        grandchild_pid = int(stdout.decode("ascii"))
        cleanup = summary["process_cleanup"]
        self.assertIn(grandchild_pid, cleanup["descendant_pids_seen"])
        self.assertTrue(cleanup["root_reaped"])
        self.assertNotIn(grandchild_pid, cleanup["remaining_descendant_pids"])
        self.assertEqual(cleanup["cleanup_errors"], [])
        self.assertFalse(psutil.pid_exists(grandchild_pid))

    def test_normal_exit_is_distinct_from_child_failure_and_limit(self):
        with tempfile.TemporaryDirectory() as temp:
            status, summary, stdout = self.execute(Path(temp), "print('ready')")
        self.assertEqual(status, 0)
        self.assertEqual(summary["runner_status"], "completed")
        self.assertEqual(summary["resource_limit"]["max_private_mib"], None)
        self.assertIn(b"ready", stdout)

    def test_nonzero_child_exit_is_not_a_timeout_or_resource_limit(self):
        with tempfile.TemporaryDirectory() as temp:
            status, summary, _ = self.execute(Path(temp), "raise SystemExit(7)")
        self.assertEqual(status, 7)
        self.assertEqual(summary["runner_status"], "child_failed")
        self.assertFalse(summary["timed_out"])
        self.assertFalse(summary["resource_limit"]["exceeded"])

    def test_fast_large_exception_exit_codes_remain_child_failures(self):
        for attempt in range(5):
            with self.subTest(attempt=attempt), tempfile.TemporaryDirectory() as temp:
                status, summary, _ = self.execute(
                    Path(temp), "raise SystemExit(3221226505)", timeout=3
                )
            self.assertEqual(summary["runner_status"], "child_failed")
            self.assertEqual(summary["exit_code"], summary["runner_exit_code"])
            self.assertFalse(summary["timed_out"])
            self.assertFalse(summary["resource_limit"]["exceeded"])
            self.assertEqual(status, summary["runner_exit_code"])

    def test_input_mutation_is_detected_after_child_exits(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            input_path = root / "input.log"
            input_path.write_bytes(b"before")
            code = f"open({str(input_path)!r}, 'ab').write(b'after')"
            status, summary, _ = self.execute(root, code, input_path=input_path)
        self.assertEqual(status, 3)
        self.assertEqual(summary["runner_status"], "input_changed")
        self.assertTrue(summary["input"]["changed"])

    def test_timeout_is_not_reported_as_resource_limit(self):
        with tempfile.TemporaryDirectory() as temp:
            status, summary, _ = self.execute(Path(temp), "import time; time.sleep(5)", timeout=0.15)
        self.assertEqual(status, 124)
        self.assertEqual(summary["runner_status"], "timeout")
        self.assertTrue(summary["timed_out"])
        self.assertFalse(summary["resource_limit"]["exceeded"])

    def test_nonpositive_limit_is_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                run_probe.parse_args([
                    "--exe", sys.executable, "--output", "unused", "--timeout", "1",
                    "--max-private-mib", "0",
                ])


if __name__ == "__main__":
    unittest.main()
