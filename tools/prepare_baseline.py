"""Fetch and verify the pinned source archive, without importing Git history."""
import hashlib
import json
from pathlib import Path
import urllib.request
import zipfile


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
                target = (destination / entry.filename).resolve()
                if not target.is_relative_to(destination.resolve()):
                    raise RuntimeError("Archive path escapes baseline directory")
            source.extractall(destination)
    # Check existing baselines too; a prior failed extraction or local edit must
    # never be mistaken for the pristine reference.
    with zipfile.ZipFile(archive) as source:
        entries = [entry for entry in source.infolist() if not entry.is_dir()]
        for entry in entries:
            expected = hashlib.sha256(source.read(entry)).digest()
            target = destination / entry.filename
            with target.open("rb") as current:
                observed = hashlib.file_digest(current, "sha256").digest()
            if expected != observed:
                raise RuntimeError(f"Baseline differs from archive: {entry.filename}")
    print(json.dumps({"baseline": str(snapshot), "archive_sha256": actual_hash, "verified_files": len(entries)}))


if __name__ == "__main__":
    main()
