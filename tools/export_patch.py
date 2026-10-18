#!/usr/bin/env python3
"""Export and verify a patch against the pinned, pristine source snapshot."""
import argparse
import difflib
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / '.local/baseline/klogg-25c7de6d8f6da2ce6a00882e5af70b4f331af4c5'
CANDIDATE = ROOT / 'upstream/klogg'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'patches/long-line-reliability.patch')
    args = parser.parse_args()
    if not BASE.is_dir() or not CANDIDATE.is_dir():
        parser.error('prepare the pristine baseline and candidate source first')
    names = sorted({p.relative_to(d).as_posix() for d in (BASE, CANDIDATE)
                    for p in d.rglob('*') if p.is_file()})
    changes, chunks = [], []
    for name in names:
        before, after = BASE / name, CANDIDATE / name
        old = before.read_bytes() if before.exists() else b''
        new = after.read_bytes() if after.exists() else b''
        if old == new:
            continue
        # The production change is source text; fail rather than silently omit a binary.
        a = old.decode('utf-8').replace('\r\n', '\n').splitlines(keepends=True)
        b = new.decode('utf-8').replace('\r\n', '\n').splitlines(keepends=True)
        diff = difflib.unified_diff(a, b,
            fromfile='a/' + name if before.exists() else '/dev/null',
            tofile='b/' + name if after.exists() else '/dev/null')
        for line in diff:
            chunks.append(line if line.endswith('\n') else line + '\n\\ No newline at end of file\n')
        changes.append({'path': name, 'before_sha256': hashlib.sha256(old).hexdigest() if before.exists() else None,
                        'after_sha256': hashlib.sha256(new).hexdigest() if after.exists() else None})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(''.join(chunks), encoding='utf-8', newline='\n')
    # --check is read-only. --ignore-space-change accepts pristine ZIP CRLF files.
    subprocess.run(['git', 'apply', '--check', '--ignore-space-change', str(args.output.resolve())],
                   cwd=BASE, check=True)
    report = {'baseline_commit': '25c7de6d8f6da2ce6a00882e5af70b4f331af4c5',
              'patch_sha256': hashlib.sha256(args.output.read_bytes()).hexdigest(),
              'git_apply_check': 'passed', 'changed_files': changes}
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'{len(changes)} changed source files; git apply --check passed: {args.output}')


if __name__ == '__main__':
    main()
