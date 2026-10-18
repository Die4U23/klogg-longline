import unittest

from tools.compare_search import validate_native_result


def native_result(**overrides):
    result = {
        "status": "passed",
        "matched_lines_count": "2",
        "matched_lines_zero_based": ["1", "4"],
        "line_list_truncated": False,
    }
    result.update(overrides)
    return result


class SearchComparisonContractTests(unittest.TestCase):
    def setUp(self):
        self.expected_lines = [1, 4]
        self.expected_offsets = [3, 20]

    def test_baseline_without_new_fields_claims_only_line_set_comparison(self):
        record, valid = validate_native_result(
            native_result(), self.expected_lines, self.expected_offsets, False
        )
        self.assertTrue(valid)
        self.assertEqual(record["comparison_scope"], "matching_line_set_only")
        self.assertEqual(record["terminal_state_validation"], "not_provided")
        self.assertEqual(record["first_hit_offset_validation"], "not_provided")

    def test_legacy_null_offsets_are_unsupported_not_a_failure(self):
        record, valid = validate_native_result(
            native_result(search_path="legacy", search_completion="complete",
                          first_hit_byte_offsets=[None, None]),
            self.expected_lines, self.expected_offsets, True,
        )
        self.assertTrue(valid)
        self.assertEqual(record["status"], "matched")
        self.assertEqual(record["first_hit_offset_validation"], "unsupported_by_legacy_path")

    def test_bounded_candidate_requires_complete_state_and_matching_offsets(self):
        good = native_result(search_path="bounded_literal", search_completion="complete",
                             first_hit_byte_offsets=["3", "20"])
        record, valid = validate_native_result(good, self.expected_lines, self.expected_offsets, True)
        self.assertTrue(valid)
        self.assertEqual(record["first_hit_offset_validation"], "matched")

        bad_offset = native_result(search_path="bounded_literal", search_completion="complete",
                                   first_hit_byte_offsets=["2", "20"])
        record, valid = validate_native_result(bad_offset, self.expected_lines, self.expected_offsets, True)
        self.assertFalse(valid)
        self.assertEqual(record["status"], "first_hit_offset_mismatch")

    def test_candidate_partial_terminal_state_fails_even_when_line_set_matches(self):
        record, valid = validate_native_result(
            native_result(search_path="legacy", search_completion="partial",
                          first_hit_byte_offsets=[None, None]),
            self.expected_lines, self.expected_offsets, True,
        )
        self.assertFalse(valid)
        self.assertEqual(record["status"], "candidate_not_complete")

    def test_bounded_path_rejects_missing_offset_values(self):
        record, valid = validate_native_result(
            native_result(search_path="bounded_literal", search_completion="complete",
                          first_hit_byte_offsets=["3", None]),
            self.expected_lines, self.expected_offsets, True,
        )
        self.assertFalse(valid)
        self.assertEqual(record["status"], "first_hit_offset_missing_for_result")


if __name__ == "__main__":
    unittest.main()
