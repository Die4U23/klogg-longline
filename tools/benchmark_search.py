#!/usr/bin/env python3
"""Interleave baseline/candidate ordinary-log throughput runs with oracle checks.

This driver only targets the deterministic short-line benchmark fixture. It
retains each runner's raw evidence and excludes every run whose complete native
result list cannot be verified against the independent byte oracle.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import statistics
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tools.fixtures import DEFAULT_CHUNK_BYTES, search_literal  # noqa: E402

RUNNER = ROOT / "tools" / "run_probe.py"
HASH_CHUNK_BYTES = 1024 * 1024
MIB = 1024 * 1024
EXPECTED_NATIVE_THREADS = 4
MAX_RESULT_LINES = 100_000


def positive_int(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a positive integer") from exc
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def sha256_file(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        while block := source.read(HASH_CHUNK_BYTES):
            digest.update(block)
            size += len(block)
    return digest.hexdigest(), size


def find_probe(directory: Path) -> Path:
    resolved = directory.expanduser().resolve()
    candidates = (
        resolved / "klogg_reliability_probe.exe",
        resolved / "reliability-probe" / "klogg_reliability_probe.exe",
        resolved / "klogg_reliability_probe",
        resolved / "reliability-probe" / "klogg_reliability_probe",
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise ValueError(f"could not find klogg_reliability_probe in {resolved}")


def interleaved_order(rounds: int) -> list[tuple[int, str]]:
    if rounds < 3:
        raise ValueError("at least three paired rounds are required")
    order = []
    for round_number in range(1, rounds + 1):
        sides = ("baseline", "candidate") if round_number % 2 else ("candidate", "baseline")
        order.extend((round_number, side) for side in sides)
    return order


def read_search_result(stdout_path: Path) -> dict[str, Any] | None:
    found = None
    with stdout_path.open("r", encoding="utf-8", errors="replace") as source:
        for raw in source:
            try:
                row = json.loads(raw)
            except json.JSONDecodeError:
                continue
            if row.get("mode") == "search" and "matched_lines_zero_based" in row:
                found = row
    return found


def oracle_expected(path: Path, pattern: str, output_path: Path) -> tuple[list[int], list[int], dict[str, Any]]:
    with output_path.open("w", encoding="utf-8", newline="\n") as output:
        summary = search_literal(path, pattern.encode("utf-8"), DEFAULT_CHUNK_BYTES, output)
    if summary["matching_lines"] > MAX_RESULT_LINES:
        raise ValueError(f"oracle hit count {summary['matching_lines']} exceeds verification cap {MAX_RESULT_LINES}")
    lines: list[int] = []
    offsets: list[int] = []
    with output_path.open("r", encoding="utf-8") as source:
        for raw in source:
            row = json.loads(raw)
            if row.get("type") == "hit":
                lines.append(row["line"] - 1)
                offsets.append(row["byte_offset"])
    if len(lines) != summary["matching_lines"]:
        raise RuntimeError("oracle output hit list did not match its summary count")
    return lines, offsets, summary


def validate_trial(native: dict[str, Any], expected_lines: list[int], expected_count: int,
                   expected_offsets: list[int], *, candidate: bool) -> tuple[bool, dict[str, Any]]:
    count = int(native.get("matched_lines_count", -1))
    lines = [int(value) for value in native.get("matched_lines_zero_based", [])]
    detail: dict[str, Any] = {
        "native_match_count": count,
        "reported_line_count": len(lines),
        "expected_match_count": expected_count,
        "line_list_truncated": native.get("line_list_truncated"),
    }
    if native.get("line_list_truncated") is not False:
        detail["oracle_validation"] = "rejected_truncated_or_unmarked_line_list"
        return False, detail
    if count != expected_count or len(lines) != count or lines != expected_lines:
        detail["oracle_validation"] = "mismatch"
        detail["first_mismatch"] = next(
            ((index, expected_lines[index] if index < len(expected_lines) else None,
              lines[index] if index < len(lines) else None)
             for index in range(max(len(expected_lines), len(lines)))
             if index >= len(expected_lines) or index >= len(lines) or expected_lines[index] != lines[index]), None)
        return False, detail

    detail["oracle_validation"] = "complete_line_list_matched"
    search_path = native.get("search_path")
    detail["search_path"] = search_path if search_path is not None else "not_emitted_by_baseline"
    if candidate:
        completion = native.get("search_completion")
        detail["search_completion"] = completion
        if completion != "complete":
            detail["oracle_validation"] = "candidate_search_not_complete"
            return False, detail
        if search_path == "bounded_literal":
            offsets = native.get("first_hit_byte_offsets")
            if not isinstance(offsets, list) or any(value is None for value in offsets):
                detail["oracle_validation"] = "candidate_offsets_incomplete"
                return False, detail
            try:
                actual_offsets = [int(value) for value in offsets]
            except (TypeError, ValueError):
                detail["oracle_validation"] = "candidate_offsets_invalid"
                return False, detail
            if actual_offsets != expected_offsets:
                detail["oracle_validation"] = "candidate_offsets_mismatch"
                detail["first_offset_mismatch"] = next(
                    ((index, expected_offsets[index], actual_offsets[index])
                     for index in range(min(len(expected_offsets), len(actual_offsets)))
                     if expected_offsets[index] != actual_offsets[index]), None)
                return False, detail
            detail["first_hit_offset_validation"] = "matched"
        elif search_path == "legacy":
            detail["first_hit_offset_validation"] = "unsupported_by_legacy_path"
        else:
            detail["oracle_validation"] = "candidate_search_path_missing_or_unsupported"
            return False, detail
    else:
        detail["search_completion"] = native.get("search_completion", "not_emitted_by_baseline")
        detail["first_hit_offset_validation"] = "not_requested_for_baseline"
    return True, detail


def measurement_ms(native: dict[str, Any], runner_summary: dict[str, Any]) -> dict[str, float]:
    index_ms = int(native["index_elapsed_ns"]) / 1_000_000
    search_ms = int(native["operation_elapsed_ns"]) / 1_000_000
    return {
        "index_ms": index_ms,
        "search_ms": search_ms,
        "total_ms": index_ms + search_ms,
        "runner_wall_ms": float(runner_summary["elapsed_seconds"]) * 1000,
    }


def summarize(trials: list[dict[str, Any]], *, threshold_percent: float,
              backend: str, threads: int, fixture_bytes: int, rounds: int) -> dict[str, Any]:
    per_side: dict[str, dict[str, Any]] = {}
    for side in ("baseline", "candidate"):
        valid = [trial for trial in trials if trial["side"] == side and trial["status"] == "verified"]
        fields = ("index_ms", "search_ms", "total_ms", "runner_wall_ms")
        medians = {
            field: statistics.median(trial["measurements_ms"][field] for trial in valid)
            for field in fields
        } if len(valid) == rounds else None
        per_side[side] = {"verified_rounds": len(valid), "median_ms": medians}
    baseline_total = per_side["baseline"]["median_ms"]
    candidate_total = per_side["candidate"]["median_ms"]
    regression = None
    over_threshold = None
    if baseline_total and candidate_total and baseline_total["total_ms"] > 0:
        regression = (candidate_total["total_ms"] / baseline_total["total_ms"] - 1) * 100
        over_threshold = regression > threshold_percent
    return {
        "format": 1,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "backend_label": backend,
        "search_thread_pool_size": threads,
        "fixture_bytes": fixture_bytes,
        "metric_definition": {
            "index_ms": "probe index_elapsed_ns / 1e6",
            "search_ms": "probe operation_elapsed_ns / 1e6",
            "total_ms": "index_ms + search_ms; excludes process launch and fixture hash passes",
            "runner_wall_ms": "runner elapsed_seconds; includes process launch and monitoring, excludes input hash passes",
            "candidate_regression_percent": "(candidate median total_ms / baseline median total_ms - 1) * 100",
        },
        "regression_threshold_percent": threshold_percent,
        "candidate_regression_percent": regression,
        "candidate_exceeds_regression_threshold": over_threshold,
        "variants": per_side,
        "all_rounds_verified": len(trials) == rounds * 2 and all(
            trial["status"] == "verified" for trial in trials
        ),
    }


def _run(options: argparse.Namespace) -> int:
    if options.rounds < 3:
        raise ValueError("--rounds must be at least 3")
    if options.threads != EXPECTED_NATIVE_THREADS:
        raise ValueError(f"this native probe fixes its search thread pool at {EXPECTED_NATIVE_THREADS}")
    manifest_path = options.manifest.expanduser().resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("profile") != "benchmark" or len(manifest.get("files", [])) != 1:
        raise ValueError("manifest must be the one-file benchmark profile")
    entry = manifest["files"][0]
    fixture = manifest_path.parent / entry["file"]
    generation = entry.get("generation", {})
    pattern = generation.get("fixed_marker")
    if not pattern or generation.get("kind") != "seeded_normal_lines":
        raise ValueError("manifest does not describe the ordinary-line benchmark fixture")
    expected_sha, expected_size = entry["sha256"], entry["length_bytes"]
    actual_sha, actual_size = sha256_file(fixture)
    if (actual_sha, actual_size) != (expected_sha, expected_size):
        raise ValueError("benchmark fixture hash or size does not match its manifest")

    output = options.output.expanduser().resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ValueError(f"refusing to overwrite populated evidence directory: {output}")
    output.mkdir(parents=True, exist_ok=True)
    oracle_path = output / "oracle.jsonl"
    expected_lines, expected_offsets, oracle_summary = oracle_expected(fixture, pattern, oracle_path)
    if expected_lines != sorted(expected_lines) or len(expected_lines) != generation["matching_lines"]:
        raise ValueError("fixture marker count or oracle line ordering does not match manifest")
    first_expected, last_expected = generation["first_hit"], generation["last_hit"]
    if (not expected_lines or expected_lines[0] + 1 != first_expected["line"]
            or expected_offsets[0] != first_expected["byte_offset"]
            or expected_lines[-1] + 1 != last_expected["line"]
            or expected_offsets[-1] != last_expected["byte_offset"]):
        raise ValueError("oracle first/last marker locations do not match manifest")

    probes = {"baseline": find_probe(options.baseline_probe_dir),
              "candidate": find_probe(options.candidate_probe_dir)}
    variant_hashes = {side: sha256_file(probe)[0] for side, probe in probes.items()}
    qt_root = options.qt_root
    if qt_root is None:
        candidate = ROOT / ".local" / "Qt" / "6.7.3" / "msvc2019_64"
        if (candidate / "bin").is_dir():
            qt_root = candidate

    trial_records: list[dict[str, Any]] = []
    records_path = output / "trials.jsonl"
    with records_path.open("w", encoding="utf-8", newline="\n") as records:
        for ordinal, (round_number, side) in enumerate(interleaved_order(options.rounds), start=1):
            probe = probes[side]
            trial_name = f"run-{ordinal:02d}-round-{round_number:02d}-{side}"
            trial_dir = output / trial_name
            child_args = ["search", str(fixture), pattern, "--timeout-ms", str(round(options.timeout * 1000))]
            command = [sys.executable, str(RUNNER), "--exe", str(probe), "--output", str(trial_dir),
                       "--timeout", str(options.timeout), "--max-private-mib", str(options.max_private_mib),
                       "--input", str(fixture), "--qpa", "offscreen"]
            if qt_root is not None:
                command += ["--qt-root", str(qt_root)]
            dll_dirs = list(options.dll_dir)
            if probe.parent.name.lower() == "reliability-probe":
                build_output = probe.parent.parent / "output"
                if build_output.is_dir():
                    dll_dirs.append(build_output)
            for directory in dll_dirs:
                command += ["--dll-dir", str(directory)]
            command += ["--", *child_args]
            (trial_dir.parent / f"{trial_name}.command.json").write_text(
                json.dumps({"command": command, "side": side, "round": round_number,
                            "backend_label": options.backend, "search_thread_pool_size": options.threads,
                            "probe_sha256": variant_hashes[side]}, indent=2) + "\n", encoding="utf-8"
            )
            completed = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       text=True, encoding="utf-8", errors="replace")
            summary_path = trial_dir / "summary.json"
            runner_summary = json.loads(summary_path.read_text(encoding="utf-8")) if summary_path.exists() else None
            native = read_search_result(trial_dir / "stdout.txt") if (trial_dir / "stdout.txt").exists() else None
            record: dict[str, Any] = {
                "ordinal": ordinal, "round": round_number, "side": side,
                "probe_sha256": variant_hashes[side], "backend_label": options.backend,
                "search_thread_pool_size": options.threads, "command_file": f"{trial_name}.command.json",
                "runner_summary": f"{trial_name}/summary.json" if runner_summary else None,
                "runner_exit_code": completed.returncode,
                "runner_status": runner_summary.get("runner_status") if runner_summary else "runner_error",
                "status": "failed",
            }
            if completed.returncode != 0 or not runner_summary:
                record["failure"] = "runner_failed"
                record["runner_stderr_tail"] = completed.stderr[-2000:]
            elif native is None:
                record["failure"] = "native_search_result_missing"
            elif native.get("status") != "passed":
                record["failure"] = "native_search_failed"
                record["native_status"] = native.get("status")
            elif native.get("line_list_truncated") is not False:
                record["failure"] = "native_line_list_truncated_or_unmarked"
            elif runner_summary.get("input", {}).get("changed"):
                record["failure"] = "input_changed_during_run"
            else:
                valid, verification = validate_trial(
                    native, expected_lines, oracle_summary["matching_lines"], expected_offsets,
                    candidate=(side == "candidate"),
                )
                record["oracle_verification"] = verification
                if not valid:
                    record["failure"] = verification["oracle_validation"]
                else:
                    measurements = measurement_ms(native, runner_summary)
                    record["measurements_ms"] = measurements
                    record["status"] = "verified"
            trial_records.append(record)
            records.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")
            records.flush()

    summary = summarize(trial_records, threshold_percent=options.regression_threshold_percent,
                        backend=options.backend, threads=options.threads, fixture_bytes=expected_size,
                        rounds=options.rounds)
    summary.update({
        "fixture": str(fixture), "fixture_sha256": expected_sha, "fixture_size_bytes": expected_size,
        "pattern": pattern, "expected_matching_lines": oracle_summary["matching_lines"],
        "oracle_artifact": oracle_path.name, "trials_artifact": records_path.name,
        "probe_sha256": variant_hashes, "requested_rounds_per_variant": options.rounds,
        "verified_trials_per_variant": {side: summary["variants"][side]["verified_rounds"]
                                         for side in ("baseline", "candidate")},
    })
    report_path = output / "summary.json"
    report_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Benchmark evidence: {output}")
    print(f"Verified rounds: baseline={summary['verified_trials_per_variant']['baseline']}, "
          f"candidate={summary['verified_trials_per_variant']['candidate']}; "
          f"candidate regression={summary['candidate_regression_percent']}")
    if not summary["all_rounds_verified"]:
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-probe-dir", required=True, type=Path)
    parser.add_argument("--candidate-probe-dir", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--backend", required=True, help="same configured search backend label for both builds")
    parser.add_argument("--threads", type=positive_int, default=EXPECTED_NATIVE_THREADS,
                        help="probe thread pool size; current native probe fixes this at four")
    parser.add_argument("--rounds", type=positive_int, default=3, help="paired rounds, minimum 3")
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--max-private-mib", type=positive_int, default=512)
    parser.add_argument("--regression-threshold-percent", type=float, default=10.0)
    parser.add_argument("--qt-root", type=Path)
    parser.add_argument("--dll-dir", type=Path, action="append", default=[])
    options = parser.parse_args(argv)
    if not math.isfinite(options.timeout) or options.timeout <= 0:
        parser.error("--timeout must be finite and greater than zero")
    if not math.isfinite(options.regression_threshold_percent) or options.regression_threshold_percent < 0:
        parser.error("--regression-threshold-percent must be finite and nonnegative")
    if options.threads != EXPECTED_NATIVE_THREADS:
        parser.error(f"current probe thread pool is fixed at {EXPECTED_NATIVE_THREADS}")
    if options.rounds < 3:
        parser.error("--rounds must be at least 3")
    return _run(options)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError, subprocess.SubprocessError) as exc:
        print(f"benchmark_search: error: {exc}", file=sys.stderr)
        raise SystemExit(2)
