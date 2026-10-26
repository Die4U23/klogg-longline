#!/usr/bin/env python3
"""Export and verify a patch against a source snapshot without Git history."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / '.local/baseline/klogg-25c7de6d8f6da2ce6a00882e5af70b4f331af4c5'
CANDIDATE = ROOT / 'upstream/klogg'
BASELINE_COMMIT = '25c7de6d8f6da2ce6a00882e5af70b4f331af4c5'


def _files(root):
    return {
        path.relative_to(root).as_posix(): path
        for path in root.rglob('*')
        if path.is_file() and '.git' not in path.relative_to(root).parts
    }


def _copy_tree(source, destination):
    for relative, path in _files(source).items():
        target = destination.joinpath(*Path(relative).parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)


def _replace_worktree(root, source):
    for child in root.iterdir():
        if child.name == '.git':
            continue
        if child.is_dir() and not child.is_symlink():
            shutil.rmtree(child)
        else:
            child.unlink()
    _copy_tree(source, root)


def _git(args, cwd, env):
    return subprocess.run(['git', *args], cwd=cwd, env=env, check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def _isolated_git_env(global_config):
    env = os.environ.copy()
    env['GIT_CONFIG_NOSYSTEM'] = '1'
    env['GIT_CONFIG_GLOBAL'] = str(global_config)
    env['GIT_ATTR_NOSYSTEM'] = '1'
    for name in ('GIT_CONFIG', 'GIT_CONFIG_PARAMETERS',
                 'GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_COMMON_DIR',
                 'GIT_OBJECT_DIRECTORY', 'GIT_ALTERNATE_OBJECT_DIRECTORIES'):
        env.pop(name, None)
    # Ignore inherited per-process git configuration (for example build URL rewrites).
    for name in list(env):
        if name == 'GIT_CONFIG_COUNT' or name.startswith('GIT_CONFIG_KEY_') or name.startswith('GIT_CONFIG_VALUE_'):
            env.pop(name, None)
    return env


def _write_pathspecs(path, names):
    # Literal NUL-delimited pathspecs avoid shell limits and glob interpretation.
    path.write_bytes(b''.join(b':(literal)' + os.fsencode(name) + b'\0' for name in names))


def export_patch(base, candidate, output, scratch_parent):
    """Export a byte-faithful patch and verify it by applying it in fresh Git."""
    base, candidate, output, scratch_parent = map(Path, (base, candidate, output, scratch_parent))
    if not base.is_dir() or not candidate.is_dir():
        raise ValueError('base and candidate must be existing directories')
    historical_patch = ROOT / 'patches/long-line-reliability.patch'
    if output.resolve() == historical_patch.resolve():
        raise ValueError('Refusing to overwrite the historical patch; pass a new --output path')
    report_path = output.with_suffix('.json')
    if output.resolve() == report_path.resolve():
        raise ValueError('Patch output and JSON report must use different paths; use a .patch output')
    if output.exists() or report_path.exists():
        raise FileExistsError(f'Refusing to overwrite existing patch or report: {output} / {report_path}')
    scratch_parent.mkdir(parents=True, exist_ok=True)
    base_files = _files(base)
    candidate_files = _files(candidate)
    names = sorted(set(base_files) | set(candidate_files))
    changes = []
    for name in names:
        before = base_files.get(name)
        after = candidate_files.get(name)
        old = before.read_bytes() if before else b''
        new = after.read_bytes() if after else b''
        if before and after and old == new:
            continue
        # Keep the established policy: production source changes must be UTF-8 text.
        if before:
            old.decode('utf-8')
        if after:
            new.decode('utf-8')
        changes.append({
            'path': name,
            'before_sha256': hashlib.sha256(old).hexdigest() if before else None,
            'after_sha256': hashlib.sha256(new).hexdigest() if after else None,
        })

    with tempfile.TemporaryDirectory(prefix='export-patch-', dir=scratch_parent) as temporary:
        temporary = Path(temporary)
        worktree = temporary / 'source'
        worktree.mkdir()
        _copy_tree(base, worktree)
        global_config = temporary / 'empty-git-config'
        global_config.write_text('', encoding='utf-8')
        env = _isolated_git_env(global_config)
        _git(['init', '--quiet'], worktree, env)
        _git(['config', '--local', 'core.autocrlf', 'false'], worktree, env)
        _git(['config', '--local', 'core.filemode', 'false'], worktree, env)
        baseline_pathspec = temporary / 'baseline-pathspecs'
        _write_pathspecs(baseline_pathspec, sorted(base_files))
        if base_files:
            _git(['add', '-f', f'--pathspec-from-file={baseline_pathspec}', '--pathspec-file-nul'], worktree, env)

        _replace_worktree(worktree, candidate)
        new_names = sorted(set(candidate_files) - set(base_files))
        if new_names:
            candidate_pathspec = temporary / 'candidate-pathspecs'
            _write_pathspecs(candidate_pathspec, new_names)
            _git(['add', '-N', '-f', f'--pathspec-from-file={candidate_pathspec}', '--pathspec-file-nul'], worktree, env)
        diff = _git(['diff', '--no-ext-diff', '--no-textconv', '--no-color', '--binary'], worktree, env).stdout
        patch_path = temporary / 'current-source.patch'
        patch_path.write_bytes(diff)

        _replace_worktree(worktree, base)
        _git(['apply', '--check', str(patch_path)], worktree, env)
        _git(['apply', str(patch_path)], worktree, env)

        rebuilt_files = _files(worktree)
        candidate_files = _files(candidate)
        rebuilt_names = set(rebuilt_files)
        candidate_names = set(candidate_files)
        if rebuilt_names != candidate_names:
            missing = sorted(candidate_names - rebuilt_names)
            extra = sorted(rebuilt_names - candidate_names)
            raise RuntimeError(f'Reconstructed file set differs from candidate: missing={missing}, extra={extra}')
        for name in sorted(candidate_names):
            rebuilt_hash = hashlib.sha256(rebuilt_files[name].read_bytes()).hexdigest()
            candidate_hash = hashlib.sha256(candidate_files[name].read_bytes()).hexdigest()
            if rebuilt_hash != candidate_hash:
                raise RuntimeError(f'Reconstructed source hash differs from candidate: {name}')

        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(diff)
        patch_hash = hashlib.sha256(diff).hexdigest()
        report = {
            'baseline_commit': BASELINE_COMMIT,
            'patch_sha256': patch_hash,
            'git_apply_check': 'passed',
            'reconstruction_check': 'passed',
            'reconstruction_verified_files': len(candidate_names),
            'changed_files': changes,
        }
        report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / '.local/patches/current-source.patch')
    args = parser.parse_args(argv)
    if not BASE.is_dir() or not CANDIDATE.is_dir():
        parser.error('prepare the pristine baseline and candidate source first')
    report = export_patch(BASE, CANDIDATE, args.output, ROOT / '.local')
    print(f"{len(report['changed_files'])} changed source files; reconstruction verified: {args.output}")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
