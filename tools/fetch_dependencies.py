#!/usr/bin/env python3
"""Fetch pinned GitHub source archives for local CPM overrides.

This tool intentionally provisions only packages listed in PACKAGES. It keeps
upstream archives and their resolved commit/hash metadata separate from the
source tree and never replaces an existing extraction.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import stat
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parent.parent
DEPENDENCIES = ROOT / ".local" / "dependencies"
LOCK_FILE = DEPENDENCIES / "dependency-lock.json"
OVERRIDES_FILE = ROOT / ".local" / "dependency-overrides.cmake"
MAX_ARCHIVE_BYTES = 256 * 1024 * 1024
MAX_EXTRACTED_BYTES = 2 * 1024 * 1024 * 1024

# Names match CPM's package names, including case, as inferred or explicitly
# provided in upstream/klogg/3rdparty/CMakeLists.txt. This is a source archive
# registry, not a claim that omitted submodule content is reconstructed.
PACKAGES = {
    "CRoaring": {"repository": "RoaringBitmap/CRoaring", "ref": "v4.2.1", "ref_kind": "tag", "archive_by_commit": True, "download_only": False},
    "streamvbyte": {"repository": "lemire/streamvbyte", "ref": "v1.0.0", "ref_kind": "tag", "download_only": False},
    "maddy": {"repository": "variar/maddy", "ref": "602e26613e624535e2de883b3f2c98a16729d1d4", "ref_kind": "commit", "download_only": False},
    "KF5Archive": {"repository": "variar/klogg_karchive", "ref": "f546bf6ae66a8d34b43da5a41afcfbf4e1a47906", "ref_kind": "commit", "download_only": False},
    "robin_hood": {"repository": "martinus/robin-hood-hashing", "ref": "3.11.2", "ref_kind": "tag", "download_only": False},
    "Catch2": {"repository": "catchorg/Catch2", "ref": "v2.13.8", "ref_kind": "tag", "download_only": False},
    "backward-cpp": {"repository": "bombela/backward-cpp", "ref": "v1.6", "ref_kind": "tag", "download_only": False},
    "KDSingleApplication": {"repository": "variar/KDSingleApplication", "ref": "5b30db30266f92bc01f1439777803ce8dbf16c79", "ref_kind": "commit", "download_only": True},
    "xxHash": {"repository": "Cyan4973/xxHash", "ref": "v0.8.1", "ref_kind": "tag", "download_only": True},
    "whereami": {"repository": "gpakosz/whereami", "ref": "dcb52a058dc14530ba9ae05e4339bd3ddfae0e0e", "ref_kind": "commit", "download_only": True},
    "exprtk": {"repository": "variar/klogg_exprtk", "ref": "1f9f4cd7d2620b7b24232de9ea22908d63913459", "ref_kind": "commit", "download_only": True},
    "KDToolBox": {"repository": "KDAB/KDToolBox", "ref": "6468867d1a46eabe1bcb2cd342f338fe66f06675", "ref_kind": "commit", "download_only": True},
    "efsw": {"repository": "SpartanJ/efsw", "ref": "1.4.1", "ref_kind": "tag", "download_only": False},
    "tbb": {"repository": "variar/oneTBB", "ref": "c9be1ac2930f02dea523003ed801b4489f3e6b6e", "ref_kind": "commit", "download_only": False},
    "mimalloc": {"repository": "microsoft/mimalloc", "ref": "v2.1.7", "ref_kind": "tag", "download_only": False},
}


def _request(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "klogg-dependency-fetch/1.0", "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(request, timeout=45) as response:
        return response.read()


def resolve_commit(repository: str, tag: str) -> str:
    api = f"https://api.github.com/repos/{repository}/git/ref/tags/{tag}"
    try:
        ref = json.loads(_request(api))
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"could not resolve GitHub tag {repository}@{tag}: {exc}") from exc
    obj = ref.get("object", {})
    if obj.get("type") == "commit":
        return obj["sha"]
    if obj.get("type") == "tag":
        try:
            annotated = json.loads(_request(f"https://api.github.com/repos/{repository}/git/tags/{obj['sha']}"))
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, KeyError) as exc:
            raise RuntimeError(f"could not resolve annotated GitHub tag {repository}@{tag}: {exc}") from exc
        target = annotated.get("object", {})
        if target.get("type") == "commit":
            return target["sha"]
    raise RuntimeError(f"GitHub tag {repository}@{tag} did not resolve to a commit")


def _download(url: str, destination: Path) -> str:
    request = urllib.request.Request(url, headers={"User-Agent": "klogg-dependency-fetch/1.0"})
    sha = hashlib.sha256()
    size = 0
    try:
        with urllib.request.urlopen(request, timeout=90) as response, destination.open("wb") as out:
            while block := response.read(1024 * 1024):
                size += len(block)
                if size > MAX_ARCHIVE_BYTES:
                    raise RuntimeError(f"archive exceeds limit of {MAX_ARCHIVE_BYTES} bytes")
                sha.update(block)
                out.write(block)
    except (urllib.error.URLError, TimeoutError) as exc:
        raise RuntimeError(f"archive download failed for {url}: {exc}") from exc
    return sha.hexdigest()


def _safe_members(archive: zipfile.ZipFile) -> tuple[str, list[tuple[zipfile.ZipInfo, str]]]:
    infos = archive.infolist()
    prefixes: set[str] = set()
    safe: list[tuple[zipfile.ZipInfo, str]] = []
    total = 0
    for info in infos:
        raw = info.filename
        # ZIP paths conventionally use '/', but reject backslashes too so a
        # Windows extraction cannot reinterpret a seemingly harmless member.
        normalized = raw.replace("\\", "/")
        path = PurePosixPath(normalized)
        if path.is_absolute() or not path.parts or any(part in {"", ".", ".."} for part in path.parts):
            raise RuntimeError(f"unsafe archive path: {raw!r}")
        if ":" in path.parts[0]:
            raise RuntimeError(f"unsafe archive drive path: {raw!r}")
        mode = (info.external_attr >> 16) & 0xFFFF
        if stat.S_ISLNK(mode):
            raise RuntimeError(f"archive contains unsupported symbolic link: {raw!r}")
        prefixes.add(path.parts[0])
        total += info.file_size
        if total > MAX_EXTRACTED_BYTES:
            raise RuntimeError(f"unpacked archive exceeds limit of {MAX_EXTRACTED_BYTES} bytes")
        relative = "/".join(path.parts[1:])
        if relative:
            safe.append((info, relative))
    if len(prefixes) != 1:
        raise RuntimeError("archive must contain one common top-level directory")
    return next(iter(prefixes)), safe


def _extract(archive_path: Path, destination: Path, download_only: bool) -> None:
    with zipfile.ZipFile(archive_path) as archive:
        _, members = _safe_members(archive)
        for info, relative in members:
            target = destination.joinpath(*PurePosixPath(relative).parts)
            resolved_parent = target.parent.resolve()
            if not resolved_parent.is_relative_to(destination.resolve()):
                raise RuntimeError(f"archive path escapes extraction directory: {info.filename!r}")
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as source, target.open("xb") as output:
                shutil.copyfileobj(source, output, length=1024 * 1024)
    if not download_only and not (destination / "CMakeLists.txt").is_file():
        raise RuntimeError(f"package source has no root CMakeLists.txt: {destination}")


def _load_lock() -> dict:
    if not LOCK_FILE.exists():
        return {"format": 1, "packages": {}}
    try:
        document = json.loads(LOCK_FILE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"cannot read dependency lock {LOCK_FILE}: {exc}") from exc
    if document.get("format") != 1 or not isinstance(document.get("packages"), dict):
        raise RuntimeError(f"unsupported dependency lock format: {LOCK_FILE}")
    return document


def _cmake_quote(value: str) -> str:
    return value.replace("\\", "/").replace('"', '\\"')


def _render_overrides(lock: dict) -> str:
    expected_lines = ["# Generated by tools/fetch_dependencies.py; edit package sources through that tool."]
    for name, entry in sorted(lock["packages"].items()):
        source = ROOT / entry["source"]
        variable = entry["cpm_source_variable"]
        expected_lines.append(f'set({variable} "{_cmake_quote(str(source.resolve()))}" CACHE PATH "Local pinned source for {name}" FORCE)')
    return "\n".join(expected_lines) + "\n"


def _lock_text(lock: dict) -> str:
    return json.dumps(lock, indent=2, sort_keys=True) + "\n"


def _validate_metadata(lock: dict) -> None:
    for path, expected in ((LOCK_FILE, _lock_text(lock)), (OVERRIDES_FILE, _render_overrides(lock))):
        if path.exists() and path.read_text(encoding="utf-8") != expected:
            raise RuntimeError(f"refusing to overwrite existing edited metadata file: {path}")


def _write_metadata(lock: dict, previous_lock: dict) -> None:
    DEPENDENCIES.mkdir(parents=True, exist_ok=True)
    _validate_metadata(previous_lock)
    replacements = ((LOCK_FILE, _lock_text(lock)), (OVERRIDES_FILE, _render_overrides(lock)))
    temporary_paths: list[tuple[Path, Path]] = []
    try:
        for target, text in replacements:
            target.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n", dir=target.parent, prefix=target.name + ".", suffix=".tmp", delete=False) as output:
                output.write(text)
                temporary_paths.append((Path(output.name), target))
        for temporary, target in temporary_paths:
            os.replace(temporary, target)
    finally:
        for temporary, _ in temporary_paths:
            temporary.unlink(missing_ok=True)


def fetch(name: str) -> dict:
    if name not in PACKAGES:
        raise ValueError(f"unsupported package {name!r}; use list to see currently enabled packages")
    package = PACKAGES[name]
    repository, ref = package["repository"], package["ref"]
    source_dir = DEPENDENCIES / name
    archive_path = DEPENDENCIES / f"{name}-{ref}.zip"
    if source_dir.exists():
        raise FileExistsError(f"refusing to replace existing package source (it may contain edits): {source_dir}")

    commit = resolve_commit(repository, ref) if package["ref_kind"] == "tag" else ref
    archive_url = (
        f"https://codeload.github.com/{repository}/zip/{commit}"
        if package.get("archive_by_commit") or package["ref_kind"] == "commit"
        else f"https://codeload.github.com/{repository}/zip/refs/tags/{ref}"
    )
    lock = _load_lock()
    previous_lock = json.loads(json.dumps(lock))
    _validate_metadata(previous_lock)
    prior = lock["packages"].get(name)
    if archive_path.exists():
        archive_sha = _file_sha256(archive_path)
        if prior and (prior.get("archive_file") != archive_path.name or prior.get("archive_url") != archive_url):
            raise RuntimeError(f"existing archive provenance differs from lock: {archive_path}")
        if prior and archive_sha != prior.get("archive_sha256"):
            raise RuntimeError(f"existing archive hash differs from lock: {archive_path}")
        with zipfile.ZipFile(archive_path) as check:
            _safe_members(check)
    else:
        with tempfile.NamedTemporaryFile(dir=DEPENDENCIES if DEPENDENCIES.exists() else ROOT / ".local", prefix="dependency-", suffix=".zip.tmp", delete=False) as temp:
            temp_path = Path(temp.name)
        try:
            archive_sha = _download(archive_url, temp_path)
            # Validate the downloaded ZIP and its extraction contract before
            # making the archive visible as a reusable artifact.
            with zipfile.ZipFile(temp_path) as check:
                _safe_members(check)
            DEPENDENCIES.mkdir(parents=True, exist_ok=True)
            if archive_path.exists():
                raise FileExistsError(archive_path)
            with temp_path.open("rb") as src, archive_path.open("xb") as dst:
                shutil.copyfileobj(src, dst, length=1024 * 1024)
        finally:
            temp_path.unlink(missing_ok=True)

    staging_parent = DEPENDENCIES
    staging_parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{name}.extract-", dir=staging_parent))
    # Extract under a nested folder because extraction expects its destination
    # to be empty, then atomically publish the complete source directory.
    extracted = staging / "source"
    extracted.mkdir()
    try:
        _extract(archive_path, extracted, package["download_only"])
        if source_dir.exists():
            raise FileExistsError(source_dir)
        os.replace(extracted, source_dir)
    finally:
        shutil.rmtree(staging, ignore_errors=True)

    lock["packages"][name] = {
        "repository": repository,
        "ref": ref,
        "ref_kind": package["ref_kind"],
        "resolved_commit": commit,
        "archive_url": archive_url,
        "archive_file": archive_path.name,
        "archive_sha256": archive_sha,
        "source": source_dir.relative_to(ROOT).as_posix(),
        "cpm_source_variable": f"CPM_{name}_SOURCE",
        "download_only": package["download_only"],
    }
    _write_metadata(lock, previous_lock)
    return lock["packages"][name]


def _file_sha256(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            sha.update(block)
    return sha.hexdigest()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list", help="list currently supported CPM source archives")
    fetch_parser = commands.add_parser("fetch", help="fetch and extract one pinned source archive")
    fetch_parser.add_argument("--name", choices=sorted(PACKAGES), action="append", required=True, help="CPM package name; may be repeated")
    args = parser.parse_args(argv)
    if args.command == "list":
        for name, package in PACKAGES.items():
            print(f"{name}\t{package['repository']}\t{package['ref']}")
        return 0
    results = [fetch(name) for name in dict.fromkeys(args.name)]
    for result in results:
        print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, zipfile.BadZipFile) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
