"""核验源码、补丁和证据的哈希来源链；不执行性能测量。"""

from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path, PurePosixPath
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
FOLLOWUP_BASE_COMMIT = "f0869dc5bd6370ea31c32469f5638cd0e2121fa5"
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def repository_file(root: Path, relative: str) -> Path:
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()) or not path.is_file():
        raise ValueError(f"仓库文件不存在或路径越界：{relative}")
    return path


def _safe_source_path(path: Any) -> str:
    if not isinstance(path, str) or not path or "\\" in path or ":" in path:
        raise ValueError(f"非法源码路径：{path!r}")
    parsed = PurePosixPath(path)
    if parsed.is_absolute() or any(part in ("", ".", "..") for part in path.split("/")):
        raise ValueError(f"源码路径越界：{path!r}")
    if not path.startswith("upstream/klogg/") or len(parsed.parts) < 3:
        raise ValueError(f"源码路径不在 upstream/klogg 下：{path!r}")
    return path


def _validate_sha(value: Any, label: str, *, nullable: bool = False) -> str | None:
    if nullable and value is None:
        return None
    if not isinstance(value, str) or SHA256_RE.fullmatch(value) is None:
        raise ValueError(f"{label} 必须是 64 位小写 SHA-256" + (" 或 null" if nullable else ""))
    return value


def _load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"JSON 根节点必须是对象：{path}")
    return value


def _apply_followups(root: Path, production: dict[str, str]) -> dict[str, str]:
    manifest_path = root / "patches/followups.json"
    if not manifest_path.exists():
        return dict(production)
    manifest = _load_json(manifest_path)
    if manifest.get("format") != 1 or manifest.get("base_commit") != FOLLOWUP_BASE_COMMIT:
        raise ValueError("后续源码覆盖清单格式或 base_commit 不受支持")
    changes = manifest.get("changes")
    if not isinstance(changes, list):
        raise ValueError("后续源码覆盖清单 changes 必须是数组")

    final = dict(production)
    seen: set[str] = set()
    for entry in changes:
        if not isinstance(entry, dict):
            raise ValueError("后续源码覆盖项必须是对象")
        path = _safe_source_path(entry.get("path"))
        if path in seen:
            raise ValueError(f"后续源码覆盖路径重复：{path}")
        seen.add(path)
        before = _validate_sha(entry.get("before_sha256"), f"{path}.before_sha256", nullable=True)
        after = _validate_sha(entry.get("after_sha256"), f"{path}.after_sha256", nullable=True)
        actual_before = final.get(path)
        if before != actual_before:
            raise ValueError(f"后续源码覆盖 before_sha256 与历史快照不匹配：{path}")
        if after is None:
            if actual_before is None:
                raise ValueError(f"后续源码覆盖试图删除未知文件：{path}")
            del final[path]
        else:
            final[path] = after
    return final


def verify_repository(root: Path = ROOT) -> dict[str, int]:
    root = root.resolve()
    patch = _load_json(root / "patches/long-line-reliability.json")
    if digest(root / "patches/long-line-reliability.patch") != patch.get("patch_sha256"):
        raise ValueError("补丁哈希与来源记录不一致")

    evidence_root = root / "evidence/windows-2026-10-17"
    # The historical manifest is mandatory; later exports must be checked too.
    manifests = {evidence_root / "manifest.json"}
    manifests.update((root / "evidence").glob("*/manifest.json"))
    copied = 0
    for manifest_path in sorted(manifests):
        manifest = _load_json(manifest_path)
        for entry in manifest["sources"]:
            if entry["status"] == "source_changed_during_export":
                raise ValueError("证据导出期间原始文件发生变化")
            if "output_path" not in entry:
                continue
            path = repository_file(root, entry["output_path"])
            if not path.is_relative_to(manifest_path.parent.resolve()):
                raise ValueError(f"证据路径越界：{entry['output_path']}")
            if path.stat().st_size != entry["output_bytes"] or digest(path) != entry["output_sha256"]:
                raise ValueError(f"导出证据完整性校验失败：{entry['output_path']}")
            copied += 1

    recorded = _load_json(evidence_root / "reproducibility/summary.json")
    production = {
        _safe_source_path(name): _validate_sha(sha, f"历史源码哈希 {name}")
        for name, sha in recorded["source_file_sha256"].items()
        if name.startswith("upstream/klogg/")
    }

    # 锁定旧补丁的 after 哈希与旧验收快照一致；后续改动只能通过单独清单叠加。
    for entry in patch["changed_files"]:
        relative = _safe_source_path("upstream/klogg/" + entry["path"])
        after = _validate_sha(entry.get("after_sha256"), f"旧补丁 after_sha256 {relative}", nullable=True)
        if production.get(relative) != after:
            raise ValueError(f"旧补丁 after_sha256 与历史源码快照不一致：{relative}")

    expected = _apply_followups(root, production)
    source_root = root / "upstream/klogg"
    current = {
        p.relative_to(root).as_posix()
        for p in source_root.rglob("*")
        if p.is_file()
    }
    if current != expected.keys():
        missing = sorted(expected.keys() - current)
        added = sorted(current - expected.keys())
        raise ValueError(f"源码文件集合与来源链不一致；缺失={missing}，未声明新增={added}")
    for name, sha in expected.items():
        if digest(repository_file(root, name)) != sha:
            raise ValueError(f"生产源码与最终来源映射不一致：{name}")
    return {"source_files": len(expected), "patch_files": len(patch["changed_files"]), "evidence_files": copied}


def main() -> int:
    try:
        result = verify_repository()
    except (OSError, KeyError, TypeError, json.JSONDecodeError, ValueError) as exc:
        print(f"完整性核验失败：{exc}", file=sys.stderr)
        return 1
    print(f"完整性核验通过：{result['source_files']} 个源码文件，{result['patch_files']} 个补丁文件，{result['evidence_files']} 份证据。")
    print("此检查只核验来源和内容完整性，不代替测试或性能验收。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
