#!/usr/bin/env python3
"""Create reproducible fixtures only inside an explicitly supplied run root."""
import argparse
import hashlib
import json
from pathlib import Path

SIZES = {"small.bin": 1024, "medium.bin": 64 * 1024, "large.bin": 16 * 1024 * 1024}


def create(root, clients=8):
    directory = Path(root) / "www" / "perf"
    directory.mkdir(parents=True, exist_ok=True)
    manifest = {}
    for name, size in SIZES.items():
        path = directory / name
        content = bytes(range(256)) * (size // 256)
        path.write_bytes(content)
        manifest[name] = {"bytes": size, "sha256": hashlib.sha256(content).hexdigest()}
    for number in range(1, clients + 1):
        content = (f"perf-client-{number:04d}\n".encode() * 128)[:1024]
        path = directory / f"client-{number}.bin"
        path.write_bytes(content)
        manifest[path.name] = {"bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()}
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (Path(root) / "uploads_storage").mkdir(exist_ok=True)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--clients", type=int, default=8)
    args = parser.parse_args()
    if not 1 <= args.clients <= 1024:
        parser.error("--clients must be 1..1024")
    print(json.dumps(create(args.root, args.clients), indent=2))


if __name__ == "__main__":
    main()
