import unittest

from tools.benchmark_search import interleaved_order, summarize, validate_trial


def result(**overrides):
    value = {
        "status": "passed",
        "matched_lines_count": "2",
        "matched_lines_zero_based": ["99", "204"],
        "line_list_truncated": False,
    }
    value.update(overrides)
    return value


def trial(side, total, *, status="verified"):
    return {
        "side": side,
        "status": status,
        "measurements_ms": {
            "index_ms": total * 0.4,
            "search_ms": total * 0.6,
            "total_ms": total,
            "runner_wall_ms": total + 10,
        },
    }


class BenchmarkSearchTests(unittest.TestCase):
    def test_runs_are_paired_and_interleaved_in_alternating_order(self):
        self.assertEqual(interleaved_order(3), [
            (1, "baseline"), (1, "candidate"),
            (2, "candidate"), (2, "baseline"),
            (3, "baseline"), (3, "candidate"),
        ])
        with self.assertRaises(ValueError):
            interleaved_order(2)

    def test_baseline_requires_complete_untruncated_line_list_oracle_match(self):
        valid, detail = validate_trial(result(), [99, 204], 2, [1000, 2000], candidate=False)
        self.assertTrue(valid)
        self.assertEqual(detail["oracle_validation"], "complete_line_list_matched")
        self.assertEqual(detail["search_completion"], "not_emitted_by_baseline")

        valid, detail = validate_trial(
            result(line_list_truncated=True), [99, 204], 2, [1000, 2000], candidate=False
        )
        self.assertFalse(valid)
        self.assertEqual(detail["oracle_validation"], "rejected_truncated_or_unmarked_line_list")

    def test_candidate_terminal_and_path_specific_offsets(self):
        valid, detail = validate_trial(
            result(search_path="legacy", search_completion="complete",
                   first_hit_byte_offsets=[None, None]),
            [99, 204], 2, [1000, 2000], candidate=True,
        )
        self.assertTrue(valid)
        self.assertEqual(detail["first_hit_offset_validation"], "unsupported_by_legacy_path")

        valid, detail = validate_trial(
            result(search_path="bounded_literal", search_completion="complete",
                   first_hit_byte_offsets=["1000", "2000"]),
            [99, 204], 2, [1000, 2000], candidate=True,
        )
        self.assertTrue(valid)
        self.assertEqual(detail["first_hit_offset_validation"], "matched")

        valid, detail = validate_trial(
            result(search_path="legacy", search_completion="partial"),
            [99, 204], 2, [1000, 2000], candidate=True,
        )
        self.assertFalse(valid)
        self.assertEqual(detail["oracle_validation"], "candidate_search_not_complete")

    def test_report_uses_medians_and_ten_percent_regression_threshold(self):
        trials = [trial("baseline", value) for value in (90, 100, 120)]
        trials += [trial("candidate", value) for value in (109, 111, 115)]
        summary = summarize(trials, threshold_percent=10, backend="standard", threads=4,
                            fixture_bytes=12345, rounds=3)
        self.assertEqual(summary["variants"]["baseline"]["median_ms"]["total_ms"], 100)
        self.assertEqual(summary["variants"]["candidate"]["median_ms"]["total_ms"], 111)
        self.assertAlmostEqual(summary["candidate_regression_percent"], 11)
        self.assertTrue(summary["candidate_exceeds_regression_threshold"])

    def test_incomplete_rounds_do_not_publish_partial_medians(self):
        trials = [trial("baseline", value) for value in (90, 100, 120)]
        trials += [trial("candidate", value) for value in (109, 111)]
        summary = summarize(trials, threshold_percent=10, backend="standard", threads=4,
                            fixture_bytes=12345, rounds=3)
        self.assertIsNone(summary["variants"]["candidate"]["median_ms"])
        self.assertIsNone(summary["candidate_regression_percent"])
        self.assertFalse(summary["all_rounds_verified"])


if __name__ == "__main__":
    unittest.main()
