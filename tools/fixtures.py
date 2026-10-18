#!/usr/bin/env python3
"""Generate deterministic long-line fixtures and independently check literal hits.

The checker is deliberately independent of the application's matcher. It reads a
fixed-size block at a time and carries at most pattern_length - 1 bytes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import sys
from pathlib import Path
from typing import BinaryIO, Iterable, TextIO

MIB = 1024 * 1024
GIB = 1024 * MIB
DEFAULT_CHUNK_BYTES = 1024 * 1024
FULL_FILE_BYTES = 5 * GIB
HIT_A = b"KLOGG_FIXTURE_HIT_A"
HIT_B = b"KLOGG_FIXTURE_HIT_B"
BENCHMARK_HIT = b"KLOGG_BENCHMARK_HIT"
BENCHMARK_TARGET_BYTES = 128 * MIB
BENCHMARK_ALPHABET = b"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _=:-"
BENCHMARK_TRANSLATION = bytes(BENCHMARK_ALPHABET[value % len(BENCHMARK_ALPHABET)] for value in range(256))


def _write_repeated(stream: BinaryIO, byte: bytes, count: int) -> None:
    block = byte * min(MIB, max(1, count))
    while count:
        part = block[: min(len(block), count)]
        stream.write(part)
        count -= len(part)


def _write_marked_line(stream: BinaryIO, size: int, markers: Iterable[tuple[int, bytes]]) -> None:
    """Write a fixed-length X-filled line, replacing selected ranges with markers."""
    ordered = sorted(markers)
    pos = 0
    for marker_pos, marker in ordered:
        if marker_pos < pos or marker_pos + len(marker) > size:
            raise ValueError("marker ranges must be ordered, non-overlapping, and inside the line")
        _write_repeated(stream, b"X", marker_pos - pos)
        stream.write(marker)
        pos = marker_pos + len(marker)
    _write_repeated(stream, b"X", size - pos)


def _digest(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(MIB):
            sha.update(block)
    return sha.hexdigest()


def _record(root: Path, name: str, description: dict, expected_hits: list[dict]) -> dict:
    path = root / name
    return {
        "file": name,
        "length_bytes": path.stat().st_size,
        "sha256": _digest(path),
        "generation": description,
        "expected_hits": expected_hits,
    }


def _write_manifest(output: Path, profile: str, seed: int, files: list[dict]) -> dict:
    manifest = {"format": 1, "profile": profile, "seed": seed, "files": files}
    with (output / "manifest.json").open("w", encoding="utf-8", newline="\n") as dest:
        json.dump(manifest, dest, ensure_ascii=False, indent=2)
        dest.write("\n")
    return manifest


def _generate_benchmark(output: Path, seed: int, line_count: int) -> dict:
    """Write seeded, variable-width ordinary lines without retaining the corpus."""
    if line_count < 1:
        raise ValueError("benchmark line count must be positive")
    rng = random.Random(seed)
    first_hit: dict | None = None
    last_hit: dict | None = None
    hit_lines = 0
    byte_offset = 0
    line_bytes_total = 0
    path = output / "benchmark_normal_lines.log"
    with path.open("wb") as dest:
        for line_number in range(1, line_count + 1):
            # The complete record, including LF, is 80..160 bytes.
            record_length = rng.randint(80, 160)
            payload_length = record_length - 1
            payload = bytearray(rng.randbytes(payload_length)).translate(BENCHMARK_TRANSLATION)
            insert_marker = line_number % 100 == 0 or line_number == line_count
            if insert_marker:
                marker_offset = 24
                payload[marker_offset:marker_offset + len(BENCHMARK_HIT)] = BENCHMARK_HIT
                hit = {"pattern": BENCHMARK_HIT.decode("ascii"), "line": line_number,
                       "byte_offset": byte_offset + marker_offset}
                if first_hit is None:
                    first_hit = hit
                last_hit = hit
                hit_lines += 1
            dest.write(payload)
            dest.write(b"\n")
            byte_offset += record_length
            line_bytes_total += record_length

    compact_hits = []
    if first_hit is not None:
        compact_hits.append(first_hit)
    if last_hit is not None and last_hit != first_hit:
        compact_hits.append(last_hit)
    entry = _record(output, path.name, {
        "kind": "seeded_normal_lines",
        "target_bytes_approx": BENCHMARK_TARGET_BYTES,
        "line_count": line_count,
        "record_bytes_including_lf": {"minimum": 80, "maximum": 160},
        "fixed_marker": BENCHMARK_HIT.decode("ascii"),
        "marker_every_lines": 100,
        "matching_lines": hit_lines,
        "first_hit": first_hit,
        "last_hit": last_hit,
        "written_bytes": line_bytes_total,
    }, compact_hits)
    return _write_manifest(output, "benchmark", seed, [entry])


def _generate_boundaries(output: Path, seed: int, chunk_bytes: int) -> dict:
    """Write compact byte-level cases that exercise reference-reader boundaries."""
    if chunk_bytes < 2:
        raise ValueError("boundary chunk size must be at least 2")
    files: list[dict] = []

    name = "chunk_boundary_targets.log"
    first_prefix = b"A" * (chunk_bytes - 1)
    first_line_length = len(first_prefix) + len(HIT_A) + 1
    second_prefix = b"B" * ((-first_line_length) % chunk_bytes)
    data = first_prefix + HIT_A + b"\n" + second_prefix + HIT_A + b"\n"
    (output / name).write_bytes(data)
    second_offset = len(first_prefix) + len(HIT_A) + 1 + len(second_prefix)
    files.append(_record(output, name, {
        "kind": "literal_targets_at_and_across_chunk_boundary",
        "chunk_bytes": chunk_bytes,
        "patterns": [HIT_A.decode("ascii"), HIT_A.decode("ascii")],
    }, [
        {"pattern": HIT_A.decode("ascii"), "line": 1, "byte_offset": chunk_bytes - 1},
        {"pattern": HIT_A.decode("ascii"), "line": 2, "byte_offset": second_offset},
    ]))

    utf8_pattern = "🙂雪".encode("utf-8")
    utf8_prefix = b"u" * (chunk_bytes - 1)
    name = "utf8_split_boundary.log"
    (output / name).write_bytes(utf8_prefix + utf8_pattern + b"\n")
    files.append(_record(output, name, {
        "kind": "valid_utf8_pattern_split_across_chunk",
        "encoding": "UTF-8", "chunk_bytes": chunk_bytes,
        "pattern": "🙂雪", "pattern_byte_length": len(utf8_pattern),
    }, [{"pattern": "🙂雪", "line": 1, "byte_offset": len(utf8_prefix)}]))

    name = "invalid_utf8_byte_oracle.log"
    invalid_data = b"prefix\xffmiddle\xfe target suffix\n"
    (output / name).write_bytes(invalid_data)
    invalid_offset = invalid_data.find(b"target")
    files.append(_record(output, name, {
        "kind": "invalid_utf8_searchable_bytes_for_reference_oracle",
        "contains_invalid_utf8": True,
        "oracle_contract": "byte search; no UTF-8 validation",
        "production_contract": "valid UTF-8 only; invalid input must fail explicitly",
    }, [{"pattern": "target", "line": 1, "byte_offset": invalid_offset}]))

    long_pattern = b"PATTERN_LONGER_THAN_CHUNK"
    name = "pattern_longer_than_chunk.log"
    (output / name).write_bytes(b"before " + long_pattern + b" after\n")
    files.append(_record(output, name, {
        "kind": "pattern_longer_than_read_chunk",
        "chunk_bytes": max(1, chunk_bytes // 2),
        "pattern": long_pattern.decode("ascii"),
        "pattern_byte_length": len(long_pattern),
    }, [{"pattern": long_pattern.decode("ascii"), "line": 1, "byte_offset": 7}]))

    name = "empty_file.log"
    (output / name).write_bytes(b"")
    files.append(_record(output, name, {"kind": "empty_file"}, []))

    name = "empty_lines_and_final_line.log"
    final = b"\n\nlast " + HIT_B
    (output / name).write_bytes(final)
    files.append(_record(output, name, {
        "kind": "empty_lines_and_final_line_without_lf",
        "line_count": 3,
    }, [{"pattern": HIT_B.decode("ascii"), "line": 3, "byte_offset": final.find(HIT_B)}]))

    name = "crlf_split_boundary.log"
    crlf_data = b"x" * (chunk_bytes - 1) + b"\r\n" + HIT_B + b"\r\n"
    (output / name).write_bytes(crlf_data)
    files.append(_record(output, name, {
        "kind": "crlf_delimiter_split_across_chunk",
        "chunk_bytes": chunk_bytes,
        "cr_byte_offset": chunk_bytes - 1,
        "lf_byte_offset": chunk_bytes,
    }, [{"pattern": HIT_B.decode("ascii"), "line": 2,
         "byte_offset": chunk_bytes + 1}]))

    return _write_manifest(output, "boundaries", seed, files)


def generate_profile(
    output: Path,
    profile: str = "smoke",
    seed: int = 1,
    *,
    sizes: dict[str, int] | None = None,
    force: bool = False,
) -> dict:
    """Generate a profile; optional sizes exist for small, fast unit tests."""
    if profile not in {"smoke", "full", "benchmark", "boundaries"}:
        raise ValueError("profile must be smoke, full, benchmark, or boundaries")
    output.mkdir(parents=True, exist_ok=True)
    sizes = sizes or {}
    if profile == "benchmark":
        planned_names = ["benchmark_normal_lines.log", "manifest.json"]
    elif profile == "boundaries":
        planned_names = ["chunk_boundary_targets.log", "utf8_split_boundary.log",
                         "invalid_utf8_byte_oracle.log", "pattern_longer_than_chunk.log",
                         "empty_file.log", "empty_lines_and_final_line.log",
                         "crlf_split_boundary.log", "manifest.json"]
    else:
        planned_names = [
        "long_1mib.log", "long_16mib.log", "long_64mib.log", "normal_lines.log",
        "mixed_long_short.log", "utf8.log", "crlf_boundary.log", "no_final_lf.log",
        "dense_hits.log", "manifest.json",
        ]
    if profile == "full":
        planned_names += ["full_5gib.log", "compressed_span_5gib.log"]
    existing = [name for name in planned_names if (output / name).exists()]
    if existing and not force:
        raise FileExistsError(f"refusing to overwrite existing fixture(s): {', '.join(existing)}; pass --force to replace them")
    rng = random.Random(seed)
    files: list[dict] = []

    if profile == "benchmark":
        line_count = sizes.get("benchmark_lines", round(BENCHMARK_TARGET_BYTES / 120))
        return _generate_benchmark(output, seed, line_count)
    if profile == "boundaries":
        return _generate_boundaries(output, seed, sizes.get("boundary_chunk_bytes", 8))

    long_sizes = [(label, sizes.get(label, n * MIB)) for label, n in (("long_1mib", 1), ("long_16mib", 16), ("long_64mib", 64))]
    for index, (label, amount) in enumerate(long_sizes):
        marker = HIT_A
        marker_pos = max(0, amount // 2 - len(marker) // 2)
        name = f"{label}.log"
        with (output / name).open("wb") as dest:
            _write_marked_line(dest, amount, [(marker_pos, marker)])
        files.append(_record(output, name, {
            "kind": "single_line", "line_bytes": amount, "fill_byte": "X",
            "markers": [{"text": marker.decode("ascii"), "byte_offset": marker_pos}],
        }, [{"pattern": marker.decode("ascii"), "line": 1, "byte_offset": marker_pos}]))

    name = "normal_lines.log"
    normal_lines = [b"INFO startup complete", b"WARN retry scheduled", b"ERROR disk unavailable"]
    normal_lines = [line + b" seed=" + str(rng.randrange(1000000)).encode("ascii") for line in normal_lines]
    with (output / name).open("wb") as dest:
        for line in normal_lines:
            dest.write(line + b"\n")
    files.append(_record(output, name, {"kind": "normal_lines", "seed": seed, "line_count": len(normal_lines)}, [
        {"pattern": "ERROR", "line": 3, "byte_offset": sum(len(line) + 1 for line in normal_lines[:2]) + normal_lines[2].find(b"ERROR")},
    ]))

    mixed_long = sizes.get("mixed_long", MIB)
    name = "mixed_long_short.log"
    long_marker_pos = max(0, mixed_long // 2 - len(HIT_B) // 2)
    with (output / name).open("wb") as dest:
        dest.write(b"short header\n")
        _write_marked_line(dest, mixed_long, [(long_marker_pos, HIT_B)])
        dest.write(b"\nshort trailer\n")
    files.append(_record(output, name, {
        "kind": "mixed_lines", "long_line_bytes": mixed_long,
        "markers": [{"text": HIT_B.decode("ascii"), "line": 2, "byte_offset": 13 + long_marker_pos}],
    }, [{"pattern": HIT_B.decode("ascii"), "line": 2, "byte_offset": 13 + long_marker_pos}]))

    name = "utf8.log"
    utf8_rows = ["启动完成🙂", "用户=李雷 状态=ready", "末尾目标=雪花❄️"]
    with (output / name).open("wb") as dest:
        for row in utf8_rows:
            dest.write(row.encode("utf-8") + b"\n")
    utf8_bytes = [row.encode("utf-8") for row in utf8_rows]
    files.append(_record(output, name, {"kind": "utf8_lines", "encoding": "UTF-8", "line_count": len(utf8_rows)}, [
        {"pattern": "雪花❄️", "line": 3, "byte_offset": sum(len(row) + 1 for row in utf8_bytes[:2]) + utf8_bytes[2].find("雪花❄️".encode("utf-8"))},
    ]))

    name = "crlf_boundary.log"
    crlf_line = b"left" + HIT_A + b"right"
    crlf_prefix = b"P" * (DEFAULT_CHUNK_BYTES - 1)
    with (output / name).open("wb") as dest:
        dest.write(crlf_prefix + b"\r\n" + crlf_line + b"\r\ntail\r\n")
    crlf_offset = len(crlf_prefix) + 2 + crlf_line.find(HIT_A)
    files.append(_record(output, name, {
        "kind": "crlf", "line_endings": "CRLF", "target_line": 2,
        "boundary": {"chunk_bytes": DEFAULT_CHUNK_BYTES, "cr_byte_offset": DEFAULT_CHUNK_BYTES - 1, "lf_byte_offset": DEFAULT_CHUNK_BYTES},
    }, [
        {"pattern": HIT_A.decode("ascii"), "line": 2, "byte_offset": crlf_offset},
    ]))

    name = "no_final_lf.log"
    final = b"first line\nfinal " + HIT_B
    with (output / name).open("wb") as dest:
        dest.write(final)
    files.append(_record(output, name, {"kind": "final_line_without_lf", "line_count": 2}, [
        {"pattern": HIT_B.decode("ascii"), "line": 2, "byte_offset": final.find(HIT_B)},
    ]))

    dense_lines = sizes.get("dense_lines", 1000)
    name = "dense_hits.log"
    with (output / name).open("wb") as dest:
        for number in range(1, dense_lines + 1):
            dest.write(f"{number:08d} {HIT_A.decode('ascii')} {HIT_A.decode('ascii')}\n".encode("ascii"))
    files.append(_record(output, name, {"kind": "dense_hits", "line_count": dense_lines, "hits_per_line": 2}, [
        {"pattern": HIT_A.decode("ascii"), "line": 1, "byte_offset": 9},
        {"pattern": HIT_A.decode("ascii"), "line": dense_lines, "byte_offset": (dense_lines - 1) * (8 + 1 + len(HIT_A) + 1 + len(HIT_A) + 1) + 9},
    ]))

    if profile == "full":
        amount = sizes.get("full_bytes", FULL_FILE_BYTES)
        boundary = sizes.get("full_boundary_bytes", 4 * GIB)
        markers = [(boundary - 32, HIT_A), (amount - len(HIT_B) - 128, HIT_B)]
        name = "full_5gib.log"
        with (output / name).open("wb") as dest:
            _write_marked_line(dest, amount, markers)
        files.append(_record(output, name, {
            "kind": "single_line_large_file", "line_bytes": amount, "fill_byte": "X",
            "markers": [{"text": marker.decode("ascii"), "byte_offset": pos} for pos, marker in markers],
            "physical_write_required": True,
        }, [
            {"pattern": HIT_A.decode("ascii"), "line": 1, "byte_offset": markers[0][0]},
            {"pattern": HIT_B.decode("ascii"), "line": 1, "byte_offset": markers[1][0]},
        ]))

        span_name = "compressed_span_5gib.log"
        span_body_bytes = amount - 128
        with (output / span_name).open("wb") as dest:
            _write_repeated(dest, b"\n", 1)
            _write_repeated(dest, b"A", span_body_bytes)
            _write_repeated(dest, b"\n", 127)
        files.append(_record(output, span_name, {
            "kind": "physical_compressed_span", "length_bytes": amount,
            "expected_first_byte_hex": "0a",
            "long_line": {
                "line": 2, "start_byte_offset": 1,
                "end_byte_offset_exclusive": 1 + span_body_bytes,
                "length_bytes": span_body_bytes, "fill_byte": "A",
            },
            "suffix": {"start_byte_offset": amount - 127, "length_bytes": 127, "fill_byte_hex": "0a"},
            "expected_final_byte_hex": "0a", "physical_write_required": True,
        }, []))

    return _write_manifest(output, profile, seed, files)


def search_literal(path: Path, pattern: bytes, chunk_bytes: int, output: TextIO) -> dict:
    """Stream first match per logical line as JSONL, with bounded input memory."""
    if not pattern:
        raise ValueError("pattern must not be empty")
    if b"\n" in pattern:
        raise ValueError("pattern must not contain LF")
    if chunk_bytes < 1:
        raise ValueError("chunk-bytes must be at least 1")

    last_emitted_line = 0
    skip_until_lf = False
    skip_from_abs = 0
    matches = 0
    carry = b""
    previous_end = 0
    line_at_chunk_start = 1
    with path.open("rb") as source:
        while block := source.read(chunk_bytes):
            chunk_start = previous_end
            data = carry + block
            data_abs = chunk_start - len(carry)
            chunk_data_start = len(carry)
            line_counted_to = chunk_data_start
            current_line = line_at_chunk_start
            if skip_until_lf:
                skip_from_local = max(0, skip_from_abs - data_abs)
                boundary = data.find(b"\n", skip_from_local)
                if boundary < 0:
                    search_from = len(data)
                else:
                    search_from = boundary + 1
                    if boundary >= chunk_data_start:
                        current_line += data.count(b"\n", line_counted_to, boundary + 1)
                    line_counted_to = search_from
                    skip_until_lf = False
            else:
                search_from = 0
            while True:
                local = data.find(pattern, search_from)
                if local < 0:
                    break
                absolute = data_abs + local
                # A candidate wholly before this chunk was already considered.
                if absolute + len(pattern) > chunk_start:
                    if local >= chunk_data_start:
                        current_line += data.count(b"\n", line_counted_to, local)
                        line_counted_to = local
                    line = current_line
                    if line != last_emitted_line:
                        output.write(json.dumps({"type": "hit", "line": line, "byte_offset": absolute}) + "\n")
                        last_emitted_line = line
                        matches += 1
                        # No valid pattern contains LF. CR remains source content.
                        # Once a line has a
                        # result, skip its remaining bytes and carry this state if
                        # the line continues into another read block.
                        boundary = data.find(b"\n", local)
                        if boundary < 0:
                            skip_until_lf = True
                            skip_from_abs = chunk_start + len(block)
                            break
                        if boundary >= chunk_data_start:
                            current_line += data.count(b"\n", line_counted_to, boundary + 1)
                        line_counted_to = boundary + 1
                        search_from = boundary + 1
                        continue
                search_from = local + 1  # include overlapping occurrences
            line_at_chunk_start += block.count(b"\n")
            previous_end += len(block)
            carry = data[-(len(pattern) - 1) :] if len(pattern) > 1 else b""

    summary = {"type": "summary", "file_bytes": previous_end, "matching_lines": matches, "pattern_bytes": len(pattern)}
    output.write(json.dumps(summary) + "\n")
    return summary


def _main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    generate = commands.add_parser("generate", help="write deterministic fixture files")
    generate.add_argument("--output", type=Path, required=True)
    generate.add_argument("--profile", choices=("smoke", "full", "benchmark", "boundaries"), default="smoke")
    generate.add_argument("--seed", type=int, default=1)
    generate.add_argument("--force", action="store_true", help="replace fixture files already present in the output directory")
    search = commands.add_parser("search", help="independently find first literal hit on each line")
    search.add_argument("file", type=Path)
    search.add_argument("--pattern", required=True, help="UTF-8 literal text; CR and LF are rejected")
    search.add_argument("--chunk-bytes", type=int, default=DEFAULT_CHUNK_BYTES)
    search.add_argument("--output", type=Path, help="JSONL destination; defaults to stdout")
    args = parser.parse_args()
    if args.command == "generate":
        manifest = generate_profile(args.output, args.profile, args.seed, force=args.force)
        print(json.dumps({"manifest": str((args.output / "manifest.json").resolve()), "files": len(manifest["files"])}))
        return 0

    destination: TextIO
    if args.output:
        same_path = args.output.resolve() == args.file.resolve()
        same_file = args.output.exists() and os.path.samefile(args.output, args.file)
        if same_path or same_file:
            raise ValueError("search output must not be the same file as the input")
        with args.output.open("w", encoding="utf-8", newline="\n") as destination:
            search_literal(args.file, args.pattern.encode("utf-8"), args.chunk_bytes, destination)
    else:
        search_literal(args.file, args.pattern.encode("utf-8"), args.chunk_bytes, sys.stdout)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(_main())
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
