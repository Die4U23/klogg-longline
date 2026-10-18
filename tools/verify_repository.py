"""核验待发布源码、补丁和精简证据的内容哈希，不重新执行性能测量。"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def repository_file(relative: str) -> Path:
    path = (ROOT / relative).resolve()
    if not path.is_relative_to(ROOT) or not path.is_file():
        raise ValueError(f"仓库文件不存在或路径越界：{relative}")
    return path


def main() -> None:
    patch = json.loads((ROOT / "patches/long-line-reliability.json").read_text(encoding="utf-8"))
    if digest(ROOT / "patches/long-line-reliability.patch") != patch["patch_sha256"]:
        raise ValueError("补丁哈希与来源记录不一致")
    for entry in patch["changed_files"]:
        relative = "upstream/klogg/" + entry["path"]
        if entry["after_sha256"] is None:
            if (ROOT / relative).exists():
                raise ValueError(f"补丁标记已删除的文件仍存在：{relative}")
        elif digest(repository_file(relative)) != entry["after_sha256"]:
            raise ValueError(f"源码与补丁记录不一致：{relative}")

    evidence_root = ROOT / "evidence/windows-2026-10-17"
    manifest = json.loads((evidence_root / "manifest.json").read_text(encoding="utf-8"))
    copied = 0
    for entry in manifest["sources"]:
        if entry["status"] == "source_changed_during_export":
            raise ValueError("证据导出期间原始文件发生变化")
        if "output_path" not in entry:
            continue
        path = repository_file(entry["output_path"])
        if not path.is_relative_to(evidence_root):
            raise ValueError(f"证据路径越界：{entry['output_path']}")
        if path.stat().st_size != entry["output_bytes"] or digest(path) != entry["output_sha256"]:
            raise ValueError(f"导出证据完整性校验失败：{entry['output_path']}")
        copied += 1

    recorded = json.loads((evidence_root / "reproducibility/summary.json").read_text(encoding="utf-8"))
    production = {name: sha for name, sha in recorded["source_file_sha256"].items()
                  if name.startswith("upstream/klogg/")}
    current = {p.relative_to(ROOT).as_posix() for p in (ROOT / "upstream/klogg").rglob("*") if p.is_file()}
    if current != production.keys():
        raise ValueError("生产源码文件集合与本地验收记录不一致")
    for name, sha in production.items():
        if digest(repository_file(name)) != sha:
            raise ValueError(f"生产源码与本地验收快照不一致：{name}")
    print(f"完整性核验通过：{len(production)} 个源码文件，{len(patch['changed_files'])} 个补丁文件，{copied} 份证据。")
    print("此检查只核验来源和内容完整性，不代替测试或性能验收。")


if __name__ == "__main__":
    main()
