#!/usr/bin/env python3
"""Run bounded native search probes and compare complete line lists to the byte oracle.

This tool deliberately excludes physically large fixtures and refuses to compare
truncated native result lists. It is intended for small, reviewable fixtures.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tools.fixtures import DEFAULT_CHUNK_BYTES, search_literal

RUNNER = ROOT / "tools" / "run_probe.py"
MAX_RESULT_LINES = 10_000
MAX_FILE_BYTES = 512 * 1024 * 1024


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


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


def patterns_for(entry: dict[str, Any]) -> list[str]:
    patterns = {hit["pattern"] for hit in entry.get("expected_hits", []) if hit.get("pattern")}
    generation = entry.get("generation", {})
    marker = generation.get("fixed_marker")
    if marker:
        patterns.add(marker)
    for pattern in generation.get("patterns", []):
        if pattern:
            patterns.add(pattern)
    return sorted(patterns)


def fixture_cases(manifest: dict[str, Any], fixture_root: Path):
    for entry in manifest.get("files", []):
        fixture_path = fixture_root / entry["file"]
        if not fixture_path.is_file():
            yield entry, None, "fixture_missing"
            continue
        generation = entry.get("generation", {})
        if generation.get("physical_write_required") or fixture_path.stat().st_size > MAX_FILE_BYTES:
            yield entry, None, "large_fixture_not_automatically_run"
            continue
        if generation.get("contains_invalid_utf8"):
            yield entry, None, "invalid_utf8_outside_production_contract"
            continue
        patterns = patterns_for(entry)
        if not patterns:
            yield entry, None, "fixture_has_no_manifest_search_pattern"
            continue
        for pattern in patterns:
            yield entry, pattern, None


def oracle_results(path: Path, pattern: str, chunk_bytes: int,
                   output_path: Path) -> tuple[list[int], list[int], dict[str, Any]]:
    with output_path.open("w", encoding="utf-8", newline="\n") as output:
        summary = search_literal(path, pattern.encode("utf-8"), chunk_bytes, output)
    if summary["matching_lines"] > MAX_RESULT_LINES:
        return [], [], summary
    lines = []
    offsets = []
    with output_path.open("r", encoding="utf-8") as source:
        for raw in source:
            row = json.loads(raw)
            if row.get("type") == "hit":
                lines.append(row["line"] - 1)
                offsets.append(row["byte_offset"])
    return lines, offsets, summary


def load_native_result(stdout_path: Path) -> dict[str, Any] | None:
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


def validate_native_result(native: dict[str, Any], expected_lines: list[int],
                           expected_offsets: list[int], require_candidate: bool) -> tuple[dict[str, Any], bool]:
    """Compare complete result rows and apply path-specific status/offset checks."""
    actual = [int(value) for value in native["matched_lines_zero_based"]]
    count = int(native["matched_lines_count"])
    record: dict[str, Any] = {
        "native_matching_lines": count,
        "compared_lines": len(expected_lines),
        "first_mismatch": next(
            ((i, expected_lines[i] if i < len(expected_lines) else None,
              actual[i] if i < len(actual) else None)
             for i in range(max(len(expected_lines), len(actual)))
             if i >= len(expected_lines) or i >= len(actual) or expected_lines[i] != actual[i]), None),
        "status": "matched" if actual == expected_lines and count == len(expected_lines) else "mismatch",
    }
    if record["status"] != "matched":
        return record, False

    completion = native.get("search_completion")
    search_path = native.get("search_path")
    offsets = native.get("first_hit_byte_offsets")
    record["comparison_scope"] = "matching_line_set_only"
    record["terminal_state_validation"] = "not_provided"
    record["first_hit_offset_validation"] = "not_provided"
    if search_path is not None:
        record["search_path"] = search_path

    if require_candidate:
        record["comparison_scope"] = "line_set_and_terminal_state"
        record["terminal_state_validation"] = completion
        if completion != "complete":
            record["status"] = "candidate_not_complete"
            return record, False
        if search_path == "legacy":
            record["first_hit_offset_validation"] = "unsupported_by_legacy_path"
            return record, True
        if search_path != "bounded_literal":
            record["status"] = "search_path_missing_or_unsupported"
            return record, False
        record["comparison_scope"] = "line_set_completion_and_first_hit_offsets"
        if not isinstance(offsets, list):
            record["status"] = "first_hit_offsets_missing"
            return record, False
        if any(value is None for value in offsets):
            record["status"] = "first_hit_offset_missing_for_result"
            record["first_hit_offset_validation"] = "missing_values"
            return record, False
        try:
            actual_offsets = [int(value) for value in offsets]
        except (TypeError, ValueError):
            actual_offsets = []
        record["first_hit_offset_validation"] = "matched" if actual_offsets == expected_offsets else "mismatch"
        if len(actual_offsets) != len(actual):
            record["status"] = "first_hit_offsets_misaligned"
            return record, False
        if actual_offsets != expected_offsets:
            record["status"] = "first_hit_offset_mismatch"
            record["first_offset_mismatch"] = next(
                ((i, expected_offsets[i], actual_offsets[i])
                 for i in range(min(len(expected_offsets), len(actual_offsets)))
                 if expected_offsets[i] != actual_offsets[i]), None)
            return record, False
        return record, True

    if completion is not None:
        record["terminal_state_validation"] = completion
    if search_path == "legacy":
        record["first_hit_offset_validation"] = "unsupported_by_legacy_path"
    elif search_path == "bounded_literal" and isinstance(offsets, list) and any(value is not None for value in offsets):
        try:
            actual_offsets = [int(value) for value in offsets]
        except (TypeError, ValueError):
            actual_offsets = []
        record["first_hit_offset_validation"] = "matched" if actual_offsets == expected_offsets else "mismatch"
        if actual_offsets != expected_offsets:
            record["status"] = "first_hit_offset_mismatch"
            return record, False
    return record, True


def compare(options: argparse.Namespace) -> int:
    probe = find_probe(options.probe_dir)
    manifest_path = options.manifest.expanduser().resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    output = options.output.expanduser().resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ValueError(f"refusing to overwrite populated output directory: {output}")
    output.mkdir(parents=True, exist_ok=True)
    records_path = output / "comparisons.jsonl"
    failures = 0
    executed = 0
    with records_path.open("w", encoding="utf-8", newline="\n") as records:
        for entry, pattern, skipped in fixture_cases(manifest, manifest_path.parent):
            if skipped:
                records.write(json.dumps({"status": "skipped", "file": entry.get("file"),
                                          "reason": skipped}, ensure_ascii=False) + "\n")
                continue
            assert pattern is not None
            path = manifest_path.parent / entry["file"]
            case_id = hashlib.sha256((entry["file"] + "\0" + pattern).encode("utf-8")).hexdigest()[:12]
            case_dir = output / case_id
            oracle_path = output / f"{case_id}.oracle.jsonl"
            expected, expected_offsets, oracle_summary = oracle_results(
                path, pattern, options.oracle_chunk_bytes, oracle_path
            )
            if oracle_summary["matching_lines"] > MAX_RESULT_LINES:
                records.write(json.dumps({"status": "skipped", "file": entry["file"],
                    "pattern": pattern, "reason": "native_result_limit_exceeded",
                    "expected_matching_lines": oracle_summary["matching_lines"]}, ensure_ascii=False) + "\n")
                continue

            child_args = ["search", str(path), pattern, "--timeout-ms", str(round(options.timeout * 1000))]
            run_command = [sys.executable, str(RUNNER), "--exe", str(probe), "--output", str(case_dir),
                           "--timeout", str(options.timeout), "--max-private-mib", str(options.max_private_mib),
                           "--input", str(path), "--qpa", "offscreen"]
            qt_root = options.qt_root
            if qt_root is None:
                default_qt_root = ROOT / ".local" / "Qt" / "6.7.3" / "msvc2019_64"
                if (default_qt_root / "bin").is_dir():
                    qt_root = default_qt_root
            if qt_root is not None:
                run_command += ["--qt-root", str(qt_root)]
            dll_dirs = list(options.dll_dir)
            if probe.parent.name.lower() == "reliability-probe":
                build_output = probe.parent.parent / "output"
                if build_output.is_dir():
                    dll_dirs.append(build_output)
            for dll_dir in dll_dirs:
                run_command += ["--dll-dir", str(dll_dir)]
            run_command += ["--", *child_args]
            completed = subprocess.run(run_command, cwd=ROOT, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
            runner_summary_path = case_dir / "summary.json"
            runner_summary = json.loads(runner_summary_path.read_text(encoding="utf-8")) if runner_summary_path.exists() else None
            record: dict[str, Any] = {
                "file": entry["file"], "pattern": pattern, "oracle_summary": oracle_summary,
                "runner_exit_code": completed.returncode,
                "runner_status": runner_summary.get("runner_status") if runner_summary else "runner_error",
                "runner_summary": str(runner_summary_path.relative_to(output)) if runner_summary else None,
                "oracle_jsonl": oracle_path.name,
            }
            if completed.returncode != 0 or not runner_summary:
                record["status"] = "runner_failed"
                record["runner_stderr"] = completed.stderr[-2000:]
                failures += 1
            else:
                native = load_native_result(case_dir / "stdout.txt")
                if native is None:
                    record["status"] = "native_result_missing"
                    failures += 1
                elif native.get("line_list_truncated"):
                    record["status"] = "native_result_truncated"
                    failures += 1
                elif runner_summary.get("input", {}).get("changed"):
                    record["status"] = "input_changed"
                    failures += 1
                elif native.get("status") != "passed":
                    record["status"] = "native_probe_failed"
                    record["native_status"] = native.get("status")
                    failures += 1
                else:
                    details, valid = validate_native_result(
                        native, expected, expected_offsets, options.candidate_status
                    )
                    record.update(details)
                    if valid:
                        executed += 1
                    else:
                        failures += 1
            records.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")
            records.flush()
    print(f"Comparison records: {records_path}")
    print(f"Completed comparisons: {executed}; failures: {failures}")
    return 1 if failures else (0 if executed else 2)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe-dir", required=True, type=Path, help="directory containing reliability probe executable")
    parser.add_argument("--manifest", required=True, type=Path, help="fixture manifest.json")
    parser.add_argument("--output", required=True, type=Path, help="new or empty comparison evidence directory")
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--max-private-mib", type=positive_int, default=512)
    parser.add_argument("--oracle-chunk-bytes", type=positive_int, default=DEFAULT_CHUNK_BYTES)
    parser.add_argument("--qt-root", type=Path, help="Qt installation root; defaults to repository-local Qt when present")
    parser.add_argument("--dll-dir", type=Path, action="append", default=[], help="additional runtime DLL directory; may repeat")
    parser.add_argument(
        "--candidate-status", action="store_true",
        help="require search_completion=complete and first_hit_byte_offsets aligned with the complete result list",
    )
    options = parser.parse_args(argv)
    if options.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    return compare(options)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError, subprocess.SubprocessError) as exc:
        print(f"compare_search: error: {exc}", file=sys.stderr)
        raise SystemExit(2)
