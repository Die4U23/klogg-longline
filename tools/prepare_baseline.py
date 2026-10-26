"""Fetch and verify the pinned source archive, without importing Git history."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import urllib.request
import zipfile


def _archive_target(filename, destination):
    """Resolve a ZIP member safely and consistently on Windows and POSIX."""
    if "\\" in filename or ":" in filename:
        raise RuntimeError(f"Archive path escapes baseline directory: {filename}")
    normalized = filename[:-1] if filename.endswith("/") else filename
    member = PurePosixPath(normalized)
    if not normalized or member.is_absolute() or any(part in ("", ".", "..") for part in normalized.split("/")):
        raise RuntimeError(f"Archive path escapes baseline directory: {filename}")
    destination = destination.resolve()
    target = (destination / Path(*member.parts)).resolve()
    if not target.is_relative_to(destination):
        raise RuntimeError("Archive path escapes baseline directory")
    return target


def verify_snapshot(source, destination, snapshot):
    """Verify the complete extracted snapshot file set and contents vs. ZIP."""
    destination = destination.resolve()
    snapshot = snapshot.resolve()
    expected = {}
    for entry in source.infolist():
        if entry.is_dir():
            continue
        target = _archive_target(entry.filename, destination)
        try:
            relative = target.relative_to(snapshot).as_posix()
        except ValueError as exc:
            raise RuntimeError(f"Archive file is outside expected snapshot: {entry.filename}") from exc
        if relative in expected:
            raise RuntimeError(f"Duplicate archive file: {entry.filename}")
        expected[relative] = hashlib.sha256(source.read(entry)).digest()

    actual_paths = {
        path.relative_to(snapshot).as_posix()
        for path in snapshot.rglob("*")
        if path.is_file()
    } if snapshot.is_dir() else set()
    expected_paths = set(expected)
    missing = sorted(expected_paths - actual_paths)
    extra = sorted(actual_paths - expected_paths)
    if missing or extra:
        raise RuntimeError(f"Baseline file set differs from archive: missing={missing}, extra={extra}")

    for relative, expected_hash in expected.items():
        target = snapshot / Path(*PurePosixPath(relative).parts)
        with target.open("rb") as current:
            observed_hash = hashlib.file_digest(current, "sha256").digest()
        if expected_hash != observed_hash:
            raise RuntimeError(f"Baseline differs from archive: {relative}")
    return len(expected)


def main():
    root = Path(__file__).resolve().parents[1]
    metadata = json.loads((root / "UPSTREAM.json").read_text(encoding="utf-8"))
    archive = root / ".local" / "downloads" / "klogg-baseline.zip"
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        temporary = archive.with_suffix(".download")
        with urllib.request.urlopen(metadata["archive_url"], timeout=120) as response, temporary.open("wb") as dest:
            while block := response.read(1024 * 1024):
                dest.write(block)
        temporary.replace(archive)
    with archive.open("rb") as source:
        actual_hash = hashlib.file_digest(source, "sha256").hexdigest()
    if actual_hash != metadata["archive_sha256"]:
        raise RuntimeError(f"Archive SHA-256 mismatch: {actual_hash}")
    destination = root / ".local" / "baseline"
    snapshot = destination / ("klogg-" + metadata["commit"])
    if not snapshot.exists():
        destination.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive) as source:
            for entry in source.infolist():
                _archive_target(entry.filename, destination)
            source.extractall(destination)
    # Check existing baselines too; a prior failed extraction or local edit must
    # never be mistaken for the pristine reference.
    with zipfile.ZipFile(archive) as source:
        for entry in source.infolist():
            _archive_target(entry.filename, destination)
        verified_files = verify_snapshot(source, destination, snapshot)
    print(json.dumps({"baseline": str(snapshot), "archive_sha256": actual_hash, "verified_files": verified_files}))


if __name__ == "__main__":
    main()
