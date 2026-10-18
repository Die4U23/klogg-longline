#!/usr/bin/env python3
"""Copy a bounded, reviewable subset of local klogg validation evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable


DEFAULT_SOURCES = {
    "baseline-m0-index": ".local/evidence/m0-index-baseline",
    "baseline-raw64": ".local/evidence/baseline/raw64",
    "baseline-gui16": ".local/evidence/baseline/gui16",
    "baseline-file-index-5gib": ".local/evidence/baseline/file-index-5gib",
    "baseline-search-matrix-unicode": ".local/evidence/baseline/search-matrix-unicode",
    "candidate-upstream-unit": ".local/evidence/candidate/upstream-unit",
    "candidate-upstream-integration": ".local/evidence/candidate/upstream-integration",
    "candidate-literal-tests-second": ".local/evidence/candidate/literal-tests-second",
    "candidate-preview-tests": ".local/evidence/candidate/preview-tests",
    "candidate-index-only-asan": ".local/evidence/index-only-asan/asan-check-20261017b",
}

GENERATOR_VERSION = "1"
TEXT_NAMES = {"stdout.txt", "stderr.txt", "compiler-version.txt"}
LABEL_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_exportable(path: Path) -> bool:
    name = path.name.lower()
    if name == "samples.jsonl" or "sample" in name:
        return False
    if name in TEXT_NAMES:
        return True
    if name in {"summary.json", "comparisons.json"} or name.startswith((
        "results-", "results.", "benchmarksummary", "benchmark-summary"
    )) or name == "results.json":
        return True
    return path.suffix.lower() == ".jsonl"


def iter_sources(source: Path) -> Iterable[Path]:
    # Walk without following directory symlinks; do not export linked files.
    for current, dirnames, filenames in os.walk(source, followlinks=False):
        dirnames[:] = sorted(d for d in dirnames if not (Path(current) / d).is_symlink())
        for filename in sorted(filenames):
            path = Path(current) / filename
            if path.is_symlink() or not path.is_file() or not is_exportable(path):
                continue
            yield path


def copy_limited(source: Path, destination: Path, limit: int, text_file: bool) -> tuple[str, int, bool]:
    """Copy at most limit bytes; text gets a marker and valid JSON is skipped upstream."""
    original_size = source.stat().st_size
    if original_size <= limit:
        shutil.copyfile(source, destination)
        return sha256_file(destination), destination.stat().st_size, False

    original_hash = sha256_file(source)
    marker = (
        f"\n[TRUNCATED by export_evidence.py; original_bytes={original_size}; "
        f"original_sha256={original_hash}; limit_bytes={limit}]\n"
    ).encode("ascii")
    if not text_file:
        raise ValueError("oversized structured JSON should have been skipped")
    if len(marker) >= limit:
        raise ValueError("text limit is too small for the truncation marker")
    payload_limit = limit - len(marker)
    with source.open("rb") as input_stream, destination.open("xb") as output_stream:
        remaining = payload_limit
        while remaining:
            chunk = input_stream.read(min(1024 * 1024, remaining))
            if not chunk:
                break
            output_stream.write(chunk)
            remaining -= len(chunk)
        output_stream.write(marker)
    return sha256_file(destination), destination.stat().st_size, True


def parse_additions(values: list[str]) -> dict[str, str]:
    additions: dict[str, str] = {}
    for value in values:
        if "=" not in value:
            raise ValueError(f"--add must be LABEL=SOURCE_RELATIVE_PATH: {value!r}")
        label, source = value.split("=", 1)
        if not LABEL_RE.fullmatch(label):
            raise ValueError(f"invalid source label: {label!r}")
        if label in additions:
            raise ValueError(f"duplicate source label: {label}")
        additions[label] = source
    return additions


def relative_posix(path: Path, root: Path) -> str:
    return path.resolve().relative_to(root.resolve()).as_posix()


def export(repo: Path, output: Path, additions: dict[str, str], text_limit: int, json_limit: int,
           no_defaults: bool = False) -> Path:
    repo = repo.resolve(strict=True)
    output = output if output.is_absolute() else repo / output
    output = output.resolve(strict=False)
    try:
        output_relative = output.relative_to(repo)
    except ValueError as exc:
        raise ValueError("output must be inside the repository so manifest paths stay portable") from exc
    local_evidence = (repo / ".local" / "evidence").resolve(strict=False)
    try:
        output.relative_to(local_evidence)
    except ValueError:
        pass
    else:
        raise ValueError("output must be outside .local/evidence")
    if output.exists():
        raise FileExistsError(f"output already exists; choose a new path: {output}")

    mapping = {} if no_defaults else dict(DEFAULT_SOURCES)
    for label, source in additions.items():
        if label in mapping:
            raise ValueError(f"source label already exists: {label}")
        mapping[label] = source

    resolved: list[tuple[str, Path]] = []
    for label, relative_source in mapping.items():
        source = (repo / relative_source).resolve(strict=True)
        try:
            source.relative_to(local_evidence)
        except ValueError as exc:
            raise ValueError(f"source must be inside .local/evidence: {relative_source}") from exc
        if not source.is_dir():
            raise NotADirectoryError(source)
        resolved.append((label, source))

    output.mkdir(parents=True)
    entries: list[dict[str, object]] = []
    try:
        for label, source_root in resolved:
            files = list(iter_sources(source_root))
            if not files:
                entries.append({
                    "label": label,
                    "source_path": relative_posix(source_root, repo),
                    "status": "no_exportable_files",
                })
            for source_file in files:
                source_rel = source_file.relative_to(source_root)
                source_size = source_file.stat().st_size
                source_hash = sha256_file(source_file)
                is_text = source_file.name.lower() in TEXT_NAMES
                limit = text_limit if is_text else json_limit
                base = {
                    "label": label,
                    "source_path": relative_posix(source_file, repo),
                    "source_bytes": source_size,
                    "source_sha256": source_hash,
                }
                if not is_text and source_size > limit:
                    entries.append({**base, "status": "skipped_too_large", "limit_bytes": limit})
                    continue

                dest_rel = Path(label) / source_rel
                destination = output / dest_rel
                destination.parent.mkdir(parents=True, exist_ok=True)
                copied_hash, copied_size, truncated = copy_limited(source_file, destination, limit, is_text)
                current_source_hash = sha256_file(source_file)
                entries.append({
                    **base,
                    "output_path": (output_relative / dest_rel).as_posix(),
                    "output_bytes": copied_size,
                    "output_sha256": copied_hash,
                    "truncated": truncated,
                    "status": "source_changed_during_export" if current_source_hash != source_hash else "copied",
                })

        manifest = {
            "schema_version": 1,
            "generator_version": GENERATOR_VERSION,
            "created_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "repository_relative_root": ".",
            "limits": {"text_bytes": text_limit, "structured_json_bytes": json_limit},
            "sources": entries,
        }
        manifest_path = output / "manifest.json"
        with manifest_path.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(manifest, stream, ensure_ascii=False, indent=2, sort_keys=True)
            stream.write("\n")
    except Exception:
        # Leave partial output intact for auditability; never silently delete evidence.
        raise
    return output


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path, required=True, help="new output directory; must not already exist")
    parser.add_argument("--add", action="append", default=[], metavar="LABEL=SOURCE", help="additional .local/evidence directory; repeatable")
    parser.add_argument(
        "--no-defaults", action="store_true",
        help="export only sources named by --add, omitting the built-in evidence groups",
    )
    parser.add_argument("--text-limit-bytes", type=int, default=2 * 1024 * 1024)
    parser.add_argument("--json-limit-bytes", type=int, default=16 * 1024 * 1024)
    args = parser.parse_args(argv)
    try:
        if args.text_limit_bytes < 512:
            raise ValueError("--text-limit-bytes must be at least 512")
        if args.json_limit_bytes <= 0:
            raise ValueError("--json-limit-bytes must be positive")
        output = export(
            args.repo_root, args.output, parse_additions(args.add),
            args.text_limit_bytes, args.json_limit_bytes, no_defaults=args.no_defaults,
        )
    except (OSError, ValueError) as exc:
        print(f"export_evidence: {exc}", file=sys.stderr)
        return 2
    print(f"Exported bounded evidence to {output}")
    print(f"Manifest: {output / 'manifest.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
