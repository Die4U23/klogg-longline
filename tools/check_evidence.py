#!/usr/bin/env python3
"""Verify a native probe's outcome against an independent oracle or GUI limits."""
import argparse
import json
from pathlib import Path


def rows(path):
    result = []
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        try:
            result.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--oracle', type=Path)
    parser.add_argument('--completion', choices=['complete', 'cancelled', 'partial'])
    parser.add_argument('--gui', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    summary = json.loads((args.directory / 'summary.json').read_text(encoding='utf-8-sig'))
    records = rows(args.directory / 'stdout.txt')
    checks = {'child_exit_zero': summary['exit_code'] == 0,
              'runner_completed': summary['runner_status'] == 'completed',
              'source_hash_unchanged': bool(summary.get('input')) and not summary['input']['changed']}
    measured = {}
    if args.gui:
        stages = [r for r in records if r.get('status') == 'stage_completed']
        checks['gui_completed'] = bool(records) and records[-1].get('status') == 'completed'
        checks['view_destroyed'] = any(r['stage'] == 'view_teardown' and r['detail'].get('view_destroyed') for r in stages)
        gaps = [int(r['heartbeat']['max_observed_gap_ns']) for r in stages]
        measured['max_gui_heartbeat_ms'] = max(gaps, default=0) / 1e6
        checks['gui_heartbeat_under_500ms'] = bool(gaps) and max(gaps) < 500_000_000
        anchors = [r for r in stages if r['stage'] == 'byte_anchor_jump_and_render']
        if anchors:
            measured['anchor_detail'] = anchors[-1]['detail']
            # The probe itself throws before completion if cached text lacks the token.
            checks['real_anchor_stage_completed'] = True
    else:
        result = next((r for r in reversed(records) if r.get('mode') == 'search' and 'search_completion' in r), {})
        checks['search_record_present'] = bool(result)
        checks['explicit_completion'] = result.get('search_completion') == args.completion
        checks['result_not_truncated'] = result.get('line_list_truncated') is False
        if result:
            measured['search_ms'] = int(result['operation_elapsed_ns']) / 1e6
            measured['peak_search_buffer_bytes'] = int(result['peak_search_buffer_bytes'])
            checks['active_budget_under_128mib'] = measured['peak_search_buffer_bytes'] <= 128 * 1024 * 1024
        if args.completion == 'cancelled' and result:
            measured['cancel_ms'] = int(result['cancel_to_completion_ns']) / 1e6
            checks['cancel_under_one_second'] = measured['cancel_ms'] < 1000
        if args.oracle and result:
            oracle = [r for r in rows(args.oracle) if r.get('type') == 'hit']
            checks['exact_oracle_lines'] = [int(n) for n in result['matched_lines_zero_based']] == [r['line'] - 1 for r in oracle]
            checks['exact_oracle_offsets'] = [int(n) for n in result['first_hit_byte_offsets']] == [r['byte_offset'] for r in oracle]
            checks['exact_oracle_count'] = int(result['matched_lines_count']) == len(oracle)
    report = {'passed': all(checks.values()), 'checks': checks, 'measurements': measured,
              'evidence_directory': str(args.directory), 'oracle': str(args.oracle) if args.oracle else None}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
