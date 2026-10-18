import io
import json
import random
import tempfile
import unittest
from unittest import mock
from pathlib import Path

from tools.fixtures import _main, generate_profile, search_literal


def hits_and_summary(text):
    rows = [json.loads(line) for line in text.getvalue().splitlines()]
    return [row for row in rows if row["type"] == "hit"], rows[-1]


class ReferenceSearchTests(unittest.TestCase):
    def search(self, contents, pattern, chunk):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "input.log"
            path.write_bytes(contents)
            result = io.StringIO()
            summary = search_literal(path, pattern, chunk, result)
            return hits_and_summary(result), summary

    def test_chunk_size_one_pattern_longer_than_chunk_and_overlapping_hits(self):
        ((hits, summary), returned) = self.search(b"ababab\n", b"abab", 1)
        self.assertEqual(hits, [{"type": "hit", "line": 1, "byte_offset": 0}])
        self.assertEqual(summary, {"type": "summary", "file_bytes": 7, "matching_lines": 1, "pattern_bytes": 4})
        self.assertEqual(returned, summary)

    def test_utf8_and_pattern_split_across_reads(self):
        text = "a🙂雪\n🙂末行".encode("utf-8")
        (hits, summary), _ = self.search(text, "🙂".encode("utf-8"), 2)
        self.assertEqual(hits, [
            {"type": "hit", "line": 1, "byte_offset": 1},
            {"type": "hit", "line": 2, "byte_offset": 9},
        ])
        self.assertEqual(summary["matching_lines"], 2)

    def test_crlf_split_empty_lines_and_final_line_without_lf(self):
        data = b"x\r\ny\r\n\r\nend"
        (hits, summary), _ = self.search(data, b"end", 2)
        self.assertEqual(hits, [{"type": "hit", "line": 4, "byte_offset": 8}])
        self.assertEqual(summary["file_bytes"], len(data))

    def test_hit_must_not_cross_lf_and_returns_first_per_line(self):
        (hits, summary), _ = self.search(b"ab\ncd ab ab\n", b"ab", 3)
        self.assertEqual(hits, [
            {"type": "hit", "line": 1, "byte_offset": 0},
            {"type": "hit", "line": 2, "byte_offset": 6},
        ])
        self.assertEqual(summary["matching_lines"], 2)

    def test_empty_or_multiline_pattern_is_rejected(self):
        for pattern in (b"", b"x\ny"):
            with self.subTest(pattern=pattern), tempfile.TemporaryDirectory() as temp:
                path = Path(temp) / "input.log"
                path.write_bytes(b"x\ny")
                with self.assertRaises(ValueError):
                    search_literal(path, pattern, 1, io.StringIO())

    def test_cr_is_content_and_lf_is_the_line_boundary(self):
        (hits, _), _ = self.search(b"x\r\ny\rz\nlast\r", b"\r", 1)
        self.assertEqual(hits, [
            {"type": "hit", "line": 1, "byte_offset": 1},
            {"type": "hit", "line": 2, "byte_offset": 4},
            {"type": "hit", "line": 3, "byte_offset": 11},
        ])

    def test_randomized_bytes_match_independent_per_line_oracle(self):
        rng = random.Random(8901)
        alphabet = b"abc\r\n"
        for case in range(80):
            data = bytes(rng.choice(alphabet) for _ in range(rng.randrange(0, 100)))
            pattern = bytes(rng.choice(b"abc") for _ in range(rng.randrange(1, 6)))
            expected = []
            line_start = 0
            line_number = 1
            for end in range(len(data) + 1):
                if end < len(data) and data[end] != 0x0A:
                    continue
                line = data[line_start:end]
                searchable = line[:-1] if line.endswith(b"\r") else line
                offset = searchable.find(pattern)
                if offset >= 0:
                    expected.append({"type": "hit", "line": line_number, "byte_offset": line_start + offset})
                line_number += 1
                line_start = end + 1
                if end == len(data):
                    break
            for chunk_bytes in (1, 2, 3, 7, 16):
                with self.subTest(case=case, chunk_bytes=chunk_bytes):
                    (hits, _), _ = self.search(data, pattern, chunk_bytes)
                    self.assertEqual(hits, expected)


class FixtureGenerationTests(unittest.TestCase):
    def test_seeded_profile_is_reproducible_and_manifest_offsets_are_real(self):
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            sizes = {"long_1mib": 37, "long_16mib": 53, "long_64mib": 71,
                     "mixed_long": 31, "dense_lines": 4}
            manifest_a = generate_profile(Path(first), seed=17, sizes=sizes)
            manifest_b = generate_profile(Path(second), seed=17, sizes=sizes)
            self.assertEqual(manifest_a, manifest_b)
            for entry in manifest_a["files"]:
                self.assertEqual((Path(first) / entry["file"]).stat().st_size, entry["length_bytes"])
                for expected in entry["expected_hits"]:
                    output = io.StringIO()
                    search_literal(Path(first) / entry["file"], expected["pattern"].encode("utf-8"), 3, output)
                    rows, _ = hits_and_summary(output)
                    self.assertIn({
                        "type": "hit", "line": expected["line"], "byte_offset": expected["byte_offset"]
                    }, rows)

    def test_existing_outputs_are_preserved_without_force(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            (output / "long_1mib.log").write_bytes(b"keep me")
            with self.assertRaises(FileExistsError):
                generate_profile(output, sizes={"long_1mib": 3})
            self.assertEqual((output / "long_1mib.log").read_bytes(), b"keep me")
            generate_profile(output, sizes={"long_1mib": 30}, force=True)
            self.assertEqual((output / "long_1mib.log").stat().st_size, 30)

    def test_reduced_full_profile_writes_both_distinct_large_file_shapes(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            amount = 4096
            manifest = generate_profile(output, profile="full", sizes={
                "full_bytes": amount,
                "full_boundary_bytes": 2048,
                "long_1mib": 32,
                "long_16mib": 32,
                "long_64mib": 32,
                "mixed_long": 32,
                "dense_lines": 2,
            })
            by_name = {entry["file"]: entry for entry in manifest["files"]}
            self.assertEqual((output / "full_5gib.log").stat().st_size, amount)
            self.assertEqual((output / "compressed_span_5gib.log").stat().st_size, amount)
            self.assertEqual((output / "compressed_span_5gib.log").read_bytes(), b"\n" + b"A" * (amount - 128) + b"\n" * 127)
            self.assertEqual(by_name["full_5gib.log"]["expected_hits"][0]["byte_offset"], 2048 - 32)
            span = by_name["compressed_span_5gib.log"]
            self.assertEqual(span["generation"]["expected_final_byte_hex"], "0a")
            self.assertEqual(span["sha256"], __import__("hashlib").sha256((output / span["file"]).read_bytes()).hexdigest())

    def test_benchmark_profile_streams_seeded_variable_width_records_and_compact_hit_index(self):
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            sizes = {"benchmark_lines": 205}
            a = generate_profile(Path(first), profile="benchmark", seed=73, sizes=sizes)
            b = generate_profile(Path(second), profile="benchmark", seed=73, sizes=sizes)
            self.assertEqual(a, b)
            entry = a["files"][0]
            path = Path(first) / entry["file"]
            raw = path.read_bytes()
            rows = raw.splitlines(keepends=True)
            self.assertEqual(len(rows), 205)
            self.assertTrue(all(80 <= len(row) <= 160 and row.endswith(b"\n") for row in rows))
            self.assertEqual(entry["length_bytes"], len(raw))
            self.assertEqual(entry["sha256"], __import__("hashlib").sha256(raw).hexdigest())
            generation = entry["generation"]
            self.assertEqual(generation["matching_lines"], 3)
            self.assertEqual(generation["first_hit"]["line"], 100)
            self.assertEqual(generation["last_hit"]["line"], 205)
            self.assertEqual([h["line"] for h in entry["expected_hits"]], [100, 205])
            output = io.StringIO()
            search_literal(path, b"KLOGG_BENCHMARK_HIT", 37, output)
            hits, summary = hits_and_summary(output)
            self.assertEqual([h["line"] for h in hits], [100, 200, 205])
            self.assertEqual(summary["matching_lines"], 3)

    def test_boundaries_profile_records_crossings_invalid_bytes_empty_cases_and_hashes(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            manifest = generate_profile(output, profile="boundaries", seed=9,
                                        sizes={"boundary_chunk_bytes": 8})
            entries = {entry["file"]: entry for entry in manifest["files"]}
            self.assertEqual(manifest["profile"], "boundaries")
            for name, entry in entries.items():
                data = (output / name).read_bytes()
                self.assertEqual(entry["length_bytes"], len(data))
                self.assertEqual(entry["sha256"], __import__("hashlib").sha256(data).hexdigest())

            crossing = entries["chunk_boundary_targets.log"]["expected_hits"]
            self.assertEqual(crossing[0]["byte_offset"] % 8, 7)
            self.assertEqual(crossing[1]["byte_offset"] % 8, 0)
            for expected in crossing:
                output_rows = io.StringIO()
                search_literal(output / "chunk_boundary_targets.log",
                               expected["pattern"].encode(), 8, output_rows)
                hits, _ = hits_and_summary(output_rows)
                self.assertIn({"type": "hit", "line": expected["line"],
                               "byte_offset": expected["byte_offset"]}, hits)

            invalid = output / "invalid_utf8_byte_oracle.log"
            self.assertIn(b"\xff", invalid.read_bytes())
            output_rows = io.StringIO()
            search_literal(invalid, b"target", 3, output_rows)
            self.assertEqual(hits_and_summary(output_rows)[0], [
                {"type": "hit", "line": 1,
                 "byte_offset": invalid.read_bytes().find(b"target")},
            ])

            long_entry = entries["pattern_longer_than_chunk.log"]
            self.assertGreater(long_entry["generation"]["pattern_byte_length"],
                               long_entry["generation"]["chunk_bytes"])
            empty_rows = io.StringIO()
            empty_summary = search_literal(output / "empty_file.log", b"x", 1, empty_rows)
            self.assertEqual(empty_summary["file_bytes"], 0)
            final_rows = io.StringIO()
            search_literal(output / "empty_lines_and_final_line.log", b"KLOGG_FIXTURE_HIT_B", 2, final_rows)
            self.assertEqual(hits_and_summary(final_rows)[0][0]["line"], 3)

    def test_search_refuses_output_path_equal_to_input(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "same.log"
            path.write_bytes(b"marker\n")
            with self.assertRaises(ValueError):
                with mock.patch("sys.argv", ["fixtures.py", "search", str(path), "--pattern", "marker", "--output", str(path)]):
                    _main()
            self.assertEqual(path.read_bytes(), b"marker\n")


if __name__ == "__main__":
    unittest.main()
