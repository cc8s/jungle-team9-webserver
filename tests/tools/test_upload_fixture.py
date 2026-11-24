#!/usr/bin/env python3
"""Execute upload_fixture.py at the complete JSON body's 1 MiB boundary."""
import base64
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

TOOL = pathlib.Path(__file__).with_name("upload_fixture.py")
LIMIT = 1024 * 1024


class UploadFixtureBoundaryTest(unittest.TestCase):
    def test_json_limit(self):
        with tempfile.TemporaryDirectory(prefix="upload-fixture-boundary-") as directory:
            root = pathlib.Path(directory)
            source = root / "fixture.bin"
            request = {"filename": source.name, "content_type": "x",
                       "encoding": "base64", "data": ""}
            overhead = len(json.dumps(request, ensure_ascii=False).encode())
            original = bytes((LIMIT - overhead) // 4 * 3)
            source.write_bytes(original)
            request["data"] = base64.b64encode(original).decode("ascii")
            gap = LIMIT - len(json.dumps(request, ensure_ascii=False).encode())
            exact_type = "x" + "a" * gap
            for delta in (-1, 0, 1):
                with self.subTest(delta=delta):
                    mime = exact_type[:-1] if delta == -1 else exact_type + "a" * delta
                    output = root / f"body-{delta}.json"
                    result = subprocess.run(
                        [sys.executable, str(TOOL), str(source), "--content-type", mime,
                         "--output", str(output)], capture_output=True, text=True)
                    if delta <= 0:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        encoded = output.read_bytes()
                        self.assertEqual(len(encoded), LIMIT + delta)
                        self.assertEqual(base64.b64decode(json.loads(encoded)["data"]), original)
                    else:
                        self.assertEqual(result.returncode, 2)
                        self.assertIn("JSON body exceeds the server's 1 MiB limit", result.stderr)
                        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
