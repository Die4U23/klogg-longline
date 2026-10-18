#!/usr/bin/env python3
"""Run one native probe process while streaming bounded resource samples to disk."""

from __future__ import annotations

import argparse
import hashlib
import math
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

SAMPLE_INTERVAL_SECONDS = 0.020
HASH_CHUNK_BYTES = 1024 * 1024
MIB = 1024 * 1024


def require_psutil() -> Any:
    try:
        import psutil  # type: ignore[import-not-found]
    except ImportError as exc:
        raise RuntimeError(
            "psutil is required for process sampling; use the repository runtime "
            "at .local/python/Scripts/python.exe or install the pinned tooling requirements"
        ) from exc
    return psutil


def parse_args(argv: list[str]) -> tuple[argparse.Namespace, list[str]]:
    try:
        separator = argv.index("--")
    except ValueError:
        separator = len(argv)
    own_args = argv[:separator]
    child_args = argv[separator + 1 :] if separator < len(argv) else []

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path, help="probe/application executable")
    parser.add_argument("--output", required=True, type=Path, help="new or empty per-run evidence directory")
    parser.add_argument("--timeout", required=True, type=float, help="maximum process runtime in seconds")
    parser.add_argument(
        "--max-private-mib", type=int,
        help="terminate the launched child if sampled private bytes exceed this positive MiB limit",
    )
    parser.add_argument("--qt-root", type=Path, help="Qt installation root; prepends its bin directory to PATH")
    parser.add_argument("--qpa", choices=("offscreen", "windows"), default="offscreen")
    parser.add_argument("--dll-dir", action="append", type=Path, default=[], help="additional DLL directory; may be repeated")
    parser.add_argument("--input", type=Path, help="optional input file to hash before and after the run")
    parser.add_argument("--compiler", help="optional compiler label for evidence metadata")
    options = parser.parse_args(own_args)
    if not math.isfinite(options.timeout) or options.timeout <= 0:
        parser.error("--timeout must be a finite value greater than zero")
    if options.max_private_mib is not None and options.max_private_mib <= 0:
        parser.error("--max-private-mib must be a positive integer")
    return options, child_args


def resolve_directory(path: Path, label: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_dir():
        raise ValueError(f"{label} is not an existing directory: {resolved}")
    return resolved


def sha256_file(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        while chunk := source.read(HASH_CHUNK_BYTES):
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def optional_int(record: Any, field: str) -> int | None:
    value = getattr(record, field, None)
    return int(value) if value is not None else None


def terminate_and_wait(process: subprocess.Popen[bytes], monitored: Any = None,
                       psutil: Any = None) -> dict[str, Any]:
    """Terminate and wait for the launched process and its observed descendants.

    Descendant Process objects come only from this exact child PID's process
    tree. All cleanup failures are returned for evidence instead of ignored.
    """
    cleanup: dict[str, Any] = {
        "root_pid": process.pid,
        "descendant_pids_seen": [],
        "descendant_pids_terminated": [],
        "descendant_pids_killed": [],
        "remaining_descendant_pids": [],
        "cleanup_errors": [],
        "root_reaped": False,
        "descendant_tracking_available": monitored is not None,
    }
    descendants: dict[int, Any] = {}

    def capture_descendants() -> None:
        if monitored is None:
            return
        try:
            for child in monitored.children(recursive=True):
                descendants[child.pid] = child
        except Exception as exc:
            if type(exc).__name__ not in {"NoSuchProcess", "ZombieProcess"}:
                cleanup["cleanup_errors"].append(f"capture descendants: {type(exc).__name__}: {exc}")

    capture_descendants()
    # Stop the parent first to prevent it from creating new children while the
    # snapshot is being cleaned. Capture once more before it fully exits.
    try:
        process.terminate()
    except OSError as exc:
        if process.poll() is None:
            cleanup["cleanup_errors"].append(f"terminate root: {type(exc).__name__}: {exc}")
    capture_descendants()

    for pid, child in descendants.items():
        try:
            child.terminate()
            cleanup["descendant_pids_terminated"].append(pid)
        except Exception as exc:
            if type(exc).__name__ not in {"NoSuchProcess", "ZombieProcess"}:
                cleanup["cleanup_errors"].append(f"terminate descendant {pid}: {type(exc).__name__}: {exc}")

    try:
        process.wait(timeout=0.5)
    except subprocess.TimeoutExpired:
        try:
            process.kill()
        except OSError as exc:
            if process.poll() is None:
                cleanup["cleanup_errors"].append(f"kill root: {type(exc).__name__}: {exc}")
        try:
            process.wait(timeout=1.0)
        except subprocess.TimeoutExpired:
            cleanup["cleanup_errors"].append("root process remained alive after kill and wait")
    cleanup["root_reaped"] = process.returncode is not None

    alive: list[Any] = []
    for pid, child in descendants.items():
        try:
            child.wait(timeout=0.5)
        except Exception as exc:
            if type(exc).__name__ == "TimeoutExpired":
                alive.append(child)
            elif type(exc).__name__ not in {"NoSuchProcess", "ZombieProcess"}:
                cleanup["cleanup_errors"].append(f"wait descendant {pid}: {type(exc).__name__}: {exc}")

    for child in alive:
        pid = child.pid
        try:
            child.kill()
            cleanup["descendant_pids_killed"].append(pid)
        except Exception as exc:
            if type(exc).__name__ not in {"NoSuchProcess", "ZombieProcess"}:
                cleanup["cleanup_errors"].append(f"kill descendant {pid}: {type(exc).__name__}: {exc}")
    for child in alive:
        try:
            child.wait(timeout=1.0)
        except Exception as exc:
            if type(exc).__name__ == "TimeoutExpired":
                cleanup["remaining_descendant_pids"].append(child.pid)
            elif type(exc).__name__ not in {"NoSuchProcess", "ZombieProcess"}:
                cleanup["cleanup_errors"].append(f"reap descendant {child.pid}: {type(exc).__name__}: {exc}")

    cleanup["descendant_pids_seen"] = sorted(descendants)
    return cleanup


def take_sample(process: Any, elapsed_seconds: float) -> dict[str, Any]:
    sample: dict[str, Any] = {
        "elapsed_seconds": round(elapsed_seconds, 6),
        "rss_bytes": None,
        "vms_bytes": None,
        "private_bytes": None,
        "peak_wset_bytes": None,
        "cpu_user_seconds": None,
        "cpu_system_seconds": None,
        "num_threads": None,
    }
    try:
        memory = process.memory_info()
        sample["rss_bytes"] = optional_int(memory, "rss")
        sample["vms_bytes"] = optional_int(memory, "vms")
        sample["private_bytes"] = optional_int(memory, "private")
        sample["peak_wset_bytes"] = optional_int(memory, "peak_wset")
        cpu = process.cpu_times()
        sample["cpu_user_seconds"] = float(cpu.user)
        sample["cpu_system_seconds"] = float(cpu.system)
        sample["num_threads"] = int(process.num_threads())
    except Exception as exc:  # Keep the run and record sampling failures as evidence.
        if type(exc).__name__ in {"NoSuchProcess", "ZombieProcess"}:
            # Popen owns the process handle and its exit code. The sampled PID
            # disappearing here is a normal poll/read race, not a runner error.
            sample["process_exit_race"] = type(exc).__name__
        else:
            sample["sample_error"] = f"{type(exc).__name__}: {exc}"
    return sample


def run(options: argparse.Namespace, child_args: list[str], psutil: Any) -> int:
    exe = options.exe.expanduser().resolve()
    if not exe.is_file():
        raise ValueError(f"Executable does not exist: {exe}")
    executable_sha256, executable_size = sha256_file(exe)
    qt_bin: Path | None = None
    if options.qt_root:
        qt_root = resolve_directory(options.qt_root, "Qt root")
        qt_bin = resolve_directory(qt_root / "bin", "Qt bin directory")

    dll_dirs = [resolve_directory(path, "DLL directory") for path in options.dll_dir]
    executable_dir = exe.parent
    path_prefix = [str(path) for path in [qt_bin, *dll_dirs, executable_dir] if path is not None]

    output = options.output.expanduser().resolve()
    if output.exists():
        if not output.is_dir():
            raise ValueError(f"Output path is not a directory: {output}")
        if any(output.iterdir()):
            raise ValueError(f"Refusing to overwrite populated evidence directory: {output}")

    input_path: Path | None = None
    input_before: dict[str, Any] | None = None
    if options.input:
        input_path = options.input.expanduser().resolve()
        if not input_path.is_file():
            raise ValueError(f"Input file does not exist: {input_path}")
        digest, size = sha256_file(input_path)
        input_before = {"path": str(input_path), "sha256_before": digest, "size_bytes_before": size}
    if not output.exists():
        output.mkdir(parents=True, exist_ok=False)

    command = [str(exe), *child_args]
    environment = os.environ.copy()
    environment["PATH"] = os.pathsep.join(path_prefix + [environment.get("PATH", "")])
    environment["QT_QPA_PLATFORM"] = options.qpa

    stdout_path = output / "stdout.txt"
    stderr_path = output / "stderr.txt"
    samples_path = output / "samples.jsonl"
    summary_path = output / "summary.json"
    started_wall = datetime.now(timezone.utc)
    started_monotonic = time.monotonic()
    timed_out = False
    resource_limit_hit = False
    resource_limit_unavailable = False
    resource_limit_measured_bytes: int | None = None
    runner_terminated_child = False
    termination_error: str | None = None
    process_cleanup: dict[str, Any] | None = None
    exit_code: int | None = None
    peaks: dict[str, int] = {}
    sample_count = 0

    with stdout_path.open("wb") as stdout_file, stderr_path.open("wb") as stderr_file, samples_path.open(
        "w", encoding="utf-8", newline="\n", buffering=1
    ) as samples_file:
        process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=stdout_file,
            stderr=stderr_file,
            env=environment,
            cwd=str(exe.parent),
        )
        monitor_init_error: str | None = None
        try:
            monitored = psutil.Process(process.pid)
        except Exception as exc:
            # Very short-lived children can exit before psutil attaches. Keep
            # polling the Popen handle and retain a sample explaining the gap.
            monitored = None
            monitor_init_error = f"{type(exc).__name__}: {exc}"
        deadline = started_monotonic + options.timeout
        try:
            while True:
                now = time.monotonic()
                if monitored is None:
                    sample = {
                        "elapsed_seconds": round(now - started_monotonic, 6),
                        "rss_bytes": None,
                        "vms_bytes": None,
                        "private_bytes": None,
                        "peak_wset_bytes": None,
                        "cpu_user_seconds": None,
                        "cpu_system_seconds": None,
                        "num_threads": None,
                        "sample_error": monitor_init_error,
                    }
                else:
                    sample = take_sample(monitored, now - started_monotonic)
                sample["utc"] = datetime.now(timezone.utc).isoformat()
                samples_file.write(json.dumps(sample, separators=(",", ":")) + "\n")
                sample_count += 1
                for key in ("rss_bytes", "vms_bytes", "private_bytes", "peak_wset_bytes"):
                    value = sample.get(key)
                    if isinstance(value, int):
                        peaks[key] = max(peaks.get(key, 0), value)
                child_code = process.poll()
                if child_code is not None:
                    # wait() reaps this exact child and preserves Windows
                    # DWORD exit codes such as 0xC0000409 (> 255).
                    exit_code = process.wait()
                    break
                if options.max_private_mib is not None:
                    measured_private = sample.get("private_bytes")
                    if isinstance(measured_private, int):
                        resource_limit_measured_bytes = measured_private
                        if measured_private > options.max_private_mib * MIB:
                            resource_limit_hit = True
                            runner_terminated_child = True
                            try:
                                process_cleanup = terminate_and_wait(process, monitored, psutil)
                            except Exception as exc:
                                termination_error = f"{type(exc).__name__}: {exc}"
                                try:
                                    process.kill()
                                except OSError:
                                    pass
                                process.wait()
                            exit_code = process.returncode
                            break
                    else:
                        # A configured hard limit cannot be claimed if this
                        # psutil build/platform provides no private-byte value.
                        resource_limit_unavailable = True
                        runner_terminated_child = True
                        termination_error = "psutil did not provide private-byte measurements"
                        try:
                            process_cleanup = terminate_and_wait(process, monitored, psutil)
                        except Exception as exc:
                            termination_error = f"{termination_error}; {type(exc).__name__}: {exc}"
                            try:
                                process.kill()
                            except OSError:
                                pass
                            process.wait()
                        exit_code = process.returncode
                        break
                if now >= deadline:
                    timed_out = True
                    runner_terminated_child = True
                    try:
                        process_cleanup = terminate_and_wait(process, monitored, psutil)
                    except Exception as exc:
                        termination_error = f"{type(exc).__name__}: {exc}"
                        try:
                            process.kill()
                        except OSError:
                            pass
                        process.wait()
                    exit_code = process.returncode
                    break
                time.sleep(min(SAMPLE_INTERVAL_SECONDS, max(0.0, deadline - time.monotonic())))
        finally:
            if process.poll() is None:
                try:
                    process_cleanup = terminate_and_wait(process, monitored, psutil)
                except Exception as exc:
                    cleanup_failure = f"{type(exc).__name__}: {exc}"
                    termination_error = f"{termination_error}; {cleanup_failure}" if termination_error else cleanup_failure
            elif exit_code is None:
                exit_code = process.wait()
            if process_cleanup:
                cleanup_errors = process_cleanup.get("cleanup_errors", [])
                remaining = process_cleanup.get("remaining_descendant_pids", [])
                if cleanup_errors or remaining:
                    detail = f"process cleanup incomplete: errors={cleanup_errors}; remaining={remaining}"
                    termination_error = f"{termination_error}; {detail}" if termination_error else detail

    process_ended_wall = datetime.now(timezone.utc)
    process_ended_monotonic = time.monotonic()
    input_after: dict[str, Any] | None = None
    if input_path:
        if input_path.is_file():
            digest, size = sha256_file(input_path)
            input_after = {
                "path": str(input_path),
                "sha256_after": digest,
                "size_bytes_after": size,
                "sha256_before": input_before["sha256_before"],
                "size_bytes_before": input_before["size_bytes_before"],
                "changed": digest != input_before["sha256_before"] or size != input_before["size_bytes_before"],
            }
        else:
            input_after = {
                "path": str(input_path),
                "sha256_after": None,
                "size_bytes_after": None,
                "sha256_before": input_before["sha256_before"],
                "size_bytes_before": input_before["size_bytes_before"],
                "changed": True,
                "exists_after": False,
            }

    if timed_out:
        runner_exit_code = 124
        runner_status = "timeout"
    elif resource_limit_hit:
        runner_exit_code = 125
        runner_status = "resource_limit"
    elif resource_limit_unavailable:
        runner_exit_code = 126
        runner_status = "resource_limit_unavailable"
    elif input_after and input_after.get("changed") and exit_code == 0:
        runner_exit_code = 3
        runner_status = "input_changed"
    elif exit_code is None:
        runner_exit_code = 1
        runner_status = "runner_error"
    else:
        runner_exit_code = exit_code
        runner_status = "completed" if exit_code == 0 else "child_failed"

    summary = {
        "schema": "klogg-process-probe-runner-v1",
        "executable": str(exe),
        "arguments": child_args,
        "command": command,
        "executable_sha256": executable_sha256,
        "executable_size_bytes": executable_size,
        "compiler": options.compiler,
        "qpa": options.qpa,
        "environment": {
            "QT_QPA_PLATFORM": options.qpa,
            "PATH_prepend": path_prefix,
        },
        "started_utc": started_wall.isoformat(),
        "ended_utc": process_ended_wall.isoformat(),
        "elapsed_seconds": round(process_ended_monotonic - started_monotonic, 6),
        "timeout_seconds": options.timeout,
        "timed_out": timed_out,
        "resource_limit": {
            "max_private_mib": options.max_private_mib,
            "threshold_bytes": options.max_private_mib * MIB if options.max_private_mib is not None else None,
            "measured_private_bytes_at_termination": resource_limit_measured_bytes,
            "exceeded": resource_limit_hit,
            "unavailable": resource_limit_unavailable,
            "runner_terminated_child": runner_terminated_child,
        },
        "exit_code": exit_code,
        "runner_exit_code": runner_exit_code,
        "runner_status": runner_status,
        "termination_error": termination_error,
        "process_cleanup": process_cleanup,
        "monitor_init_error": monitor_init_error,
        "sample_interval_seconds": SAMPLE_INTERVAL_SECONDS,
        "sample_count": sample_count,
        "peak_observed_bytes": peaks,
        "input": input_after,
        "artifacts": {
            "stdout": stdout_path.name,
            "stderr": stderr_path.name,
            "samples": samples_path.name,
        },
    }
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Evidence directory: {output}")
    print(
        f"Process exit code: {exit_code}; runner exit code: {runner_exit_code}; "
        f"status: {runner_status}; samples: {sample_count}"
    )
    return runner_exit_code


def main(argv: list[str] | None = None) -> int:
    try:
        psutil = require_psutil()
        options, child_args = parse_args(sys.argv[1:] if argv is None else argv)
        return run(options, child_args, psutil)
    except (ValueError, RuntimeError, OSError) as exc:
        print(f"run_probe: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
