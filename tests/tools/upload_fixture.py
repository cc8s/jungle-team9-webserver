#!/usr/bin/env python3
"""Create a JSON/Base64 upload fixture; optionally send with curl and verify storage.

python3 tests/tools/upload_fixture.py photo.png --output /tmp/upload.json
python3 tests/tools/upload_fixture.py photo.png --url http://localhost:8000/api/upload
The server must already be running; prepare uploads_storage yourself before uploading.
"""
import argparse
import base64
import json
import pathlib
import shlex
import stat
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("file", type=pathlib.Path)
parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("/tmp/upload.json"))
parser.add_argument("--content-type", default="application/octet-stream")
parser.add_argument("--url", help="If set, call curl and verify the saved pair")
parser.add_argument("--storage", type=pathlib.Path, default=pathlib.Path("uploads_storage"))
args = parser.parse_args()
original = args.file.read_bytes()
request = {
    "filename": args.file.name,
    "content_type": args.content_type,
    "encoding": "base64",
    "data": base64.b64encode(original).decode("ascii"),
}
encoded = json.dumps(request, ensure_ascii=False).encode("utf-8")
if len(encoded) > 1024 * 1024:
    parser.error("JSON body exceeds the server's 1 MiB limit")
args.output.write_bytes(encoded)
command = ["curl", "--silent", "--show-error", "--fail-with-body", "-H",
           "Content-Type: application/json", "--data-binary", "@" + str(args.output),
           args.url or "http://localhost:8000/api/upload"]
print("JSON bytes:", len(encoded), "original bytes:", len(original))
print(shlex.join(command))
if args.url:
    result = json.loads(subprocess.check_output(command))
    if not result.get("ok"):
        raise SystemExit("Server rejected upload: " + repr(result))
    identifier = result["data"]["id"]
    if not identifier or pathlib.Path(identifier).name != identifier:
        raise SystemExit("Unsafe server identifier")
    binary = args.storage / (identifier + ".bin")
    metadata = args.storage / (identifier + ".json")
    assert binary.read_bytes() == original, "Stored bytes differ"
    info = json.loads(metadata.read_text(encoding="utf-8"))
    assert info["id"] == identifier and info["filename"] == args.file.name
    assert info["content_type"] == args.content_type and info["size"] == len(original)
    assert stat.S_IMODE(binary.stat().st_mode) == 0o400
    assert stat.S_IMODE(metadata.stat().st_mode) == 0o400
    print("Verified byte preservation, metadata and 0400:", identifier)
