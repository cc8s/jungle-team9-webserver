#!/usr/bin/env python3
"""Check Linux tool availability, wrk wire version, product response and PID access."""
import os
from pathlib import Path
import socket
import subprocess
import threading

listener = socket.socket()
listener.bind(("127.0.0.1", 0))
listener.listen(1)
port = listener.getsockname()[1]
captured = []


def capture():
    listener.settimeout(5)
    # wrk performs a connection-only address probe before its timed request.
    while not captured:
        with listener.accept()[0] as connection:
            request = connection.recv(4096)
            if not request:
                continue
            captured.append(request.split(b"\r\n", 1)[0])
            connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
    listener.close()


worker = threading.Thread(target=capture)
worker.start()
result = subprocess.run(["wrk", "-t1", "-c1", "-d1s", f"http://127.0.0.1:{port}/"],
                        capture_output=True, text=True, timeout=10)
worker.join(timeout=6)
assert result.returncode == 0, result.stderr
assert captured == [b"GET / HTTP/1.1"], captured
with socket.create_connection(("server", 8000), timeout=5) as connection:
    connection.sendall(b"GET /perf/small.bin HTTP/1.1\r\nHost: server\r\nConnection: close\r\n\r\n")
    response = bytearray()
    while data := connection.recv(4096):
        response.extend(data)
header, body = bytes(response).split(b"\r\n\r\n", 1)
assert header.startswith(b"HTTP/1.1 200 "), header
assert body == Path("/fixtures/www/perf/small.bin").read_bytes()
assert len(body) == 1024
assert Path("/proc/1/fd").is_dir()
assert os.access("/proc/1/stat", os.R_OK)
print("environment smoke: wrk HTTP/1.1, product 200/body, server PID namespace passed")
