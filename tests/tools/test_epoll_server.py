#!/usr/bin/env python3
"""Linux actual-main integration tests; no production-only test switches."""
import concurrent.futures
import json
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
BINARY = ROOT / "webserver"
GET = b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"


def connect():
    s = socket.create_connection(("127.0.0.1", 8000), timeout=3)
    s.settimeout(8)
    return s


def response(s, carry=b""):
    data = carry
    while b"\r\n\r\n" not in data:
        chunk = s.recv(65536)
        assert chunk, data
        data += chunk
    header, data = data.split(b"\r\n\r\n", 1)
    fields = dict(line.lower().split(b":", 1) for line in header.split(b"\r\n")[1:])
    length = int(fields[b"content-length"].strip())
    while len(data) < length:
        chunk = s.recv(65536)
        assert chunk, (header, len(data), length)
        data += chunk
    return int(header.split()[1]), data[:length], data[length:], header


class Server:
    def __init__(self, directory, args=(), env=None):
        self.stop_sent = False
        self.process = subprocess.Popen([str(BINARY), *args], cwd=directory,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
        ready, _, _ = select.select([self.process.stderr], [], [], 10)
        assert ready, "server failed to start"
        line = self.process.stderr.readline()
        assert b"listening" in line, line

    def request_stop(self):
        if self.process.poll() is None and not self.stop_sent:
            self.stop_sent = True
            self.process.send_signal(signal.SIGTERM)

    def stop(self):
        self.request_stop()
        try:
            assert self.process.wait(timeout=15) == 0, self.process.stderr.read()
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.process.stderr.close()


def post(path, body, close=False):
    return (f"POST {path} HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n" + ("Connection: close\r\n" if close else "") + "\r\n").encode() + body


def main():
    assert os.uname().sysname == "Linux"
    with tempfile.TemporaryDirectory(prefix="f2-epoll-") as directory:
        work = Path(directory)
        (work / "www").mkdir()
        (work / "uploads_storage").mkdir()
        (work / "www/index.html").write_bytes(b"index\x00binary")
        file_data = bytes(range(256)) * 1024
        (work / "www/slow.bin").write_bytes(file_data)
        server = Server(work)
        try:
            # Slow headers and idle connections do not occupy workers.
            slow = connect(); slow.sendall(b"GET / HTTP/1.1\r\n")
            idle = [connect() for _ in range(6)]
            for s in idle:
                s.sendall(GET); assert response(s)[0] == 200
            with connect() as s:
                s.sendall(GET + GET)
                status, body, carry, _ = response(s)
                assert status == 200 and body == b"index\x00binary"
                assert response(s, carry)[0] == 200
            # GET body is ignored after the response; next POST boundaries survive.
            with connect() as s:
                s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\n")
                assert response(s)[0] == 200
                s.sendall(b"ab")
                s.sendall(b"cd" + post("/api/users", b'{"name":"later"}', True))
                assert response(s)[0] == 200
                assert s.recv(1) == b""
            # Empty EOF is silent; partial EOF still receives 400.
            with connect() as s:
                s.shutdown(socket.SHUT_WR)
                assert s.recv(1) == b""
            with connect() as s:
                s.sendall(b"GET / HTTP/1.1\r\nHost:")
                s.shutdown(socket.SHUT_WR)
                assert response(s)[0] == 400
                assert s.recv(1) == b""
            with connect() as s:
                s.sendall(GET); s.shutdown(socket.SHUT_WR)
                assert response(s)[0] == 200
            # The 100th response closes, even with all requests already buffered.
            with connect() as s:
                s.sendall(GET * 100)
                carry = b""
                for index in range(100):
                    status, _, carry, header = response(s, carry)
                    assert status == 200
                    if index == 99:
                        assert b"Connection: close" in header
                assert not carry and s.recv(1) == b""
            # Parallel uploads use distinct IDs and preserve bytes/metadata.
            def upload(_):
                body = b'{"filename":"x.bin","content_type":"application/octet-stream","encoding":"base64","data":"AAEC"}'
                with connect() as s:
                    s.sendall(post("/api/upload", body, True))
                    status, data, _, _ = response(s)
                    assert status == 201, data
                    return json.loads(data)["data"]["id"]
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
                ids = list(executor.map(upload, range(24)))
            assert len(set(ids)) == len(ids)
            for identifier in ids:
                assert (work / f"uploads_storage/{identifier}.bin").read_bytes() == b"\x00\x01\x02"
            # Real epoll deadlines fire despite unrelated ready events.
            slow.close()
            header_wait, body_wait, idle_wait = connect(), connect(), connect()
            header_wait.sendall(b"GET / HTTP/1.1\r\nHost:")
            body_wait.sendall(b"POST /api/users HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\n{")
            idle_wait.sendall(GET); assert response(idle_wait)[0] == 200
            for timed in (header_wait, body_wait, idle_wait): timed.settimeout(15)
            with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
                header_result = executor.submit(response, header_wait)
                body_result = executor.submit(response, body_wait)
                idle_result = executor.submit(idle_wait.recv, 1)
                until = time.monotonic() + 6
                while time.monotonic() < until:
                    with connect() as active:
                        active.sendall(GET)
                        assert response(active)[0] == 200
                    time.sleep(.01)
                assert header_result.result()[0] == 408
                assert body_result.result()[0] == 408
                assert idle_result.result() == b""
            for timed in (header_wait, body_wait, idle_wait): timed.close()
            for s in idle: s.close()
            slow.close()
        finally:
            server.stop()

        # Explicit connection cap pauses accept and resumes after slot release.
        server = Server(work, ["--max", "1"])
        try:
            a = connect(); a.sendall(GET); assert response(a)[0] == 200
            b = connect(); b.sendall(GET); b.settimeout(.2)
            try:
                assert not b.recv(1), "second connection should remain unaccepted"
                raise AssertionError("unexpected EOF")
            except socket.timeout:
                pass
            a.close(); b.settimeout(8); assert response(b)[0] == 200; b.close()
        finally:
            server.stop()

        # Buffer capacity across connections, not length, is limited to 1 MiB.
        server = Server(work, ["--buffer", "1"])
        try:
            a = connect()
            a.sendall(b"POST /api/users HTTP/1.1\r\nHost: x\r\nContent-Length: 900000\r\n\r\n" + b" " * 300000)
            time.sleep(.1)  # a has expanded to 512 KiB but remains incomplete.
            b = connect()
            b.sendall(b"POST /api/users HTTP/1.1\r\nHost: x\r\nContent-Length: 900000\r\n\r\n" + b" " * 600000)
            try:
                assert b.recv(1) == b""
            except ConnectionResetError:
                pass
            b.close()
            with connect() as s:
                s.sendall(GET); assert response(s)[0] == 200
            a.close()
        finally:
            server.stop()

        # Deterministic worker blocking tests actual 503 and SIGTERM drain.
        library = work / "read_gate.so"
        subprocess.run(["gcc", "-shared", "-fPIC", "-O2", str(ROOT / "tests/fixtures/f2_read_gate.c"),
                        "-ldl", "-o", str(library)], check=True)
        gate, entered = work / "gate", work / "entered"
        gate.touch()
        preload = os.environ.get("F2_TEST_PRELOAD_PREFIX", "")
        env = dict(os.environ, LD_PRELOAD=(preload + ":" if preload else "") + str(library),
                   F2_READ_GATE=str(gate), F2_GATE_ENTERED=str(entered))
        server = Server(work, ["--worker", "1", "--queue", "1"], env)
        sockets = []
        try:
            a = connect(); sockets.append(a)
            a.sendall(b"GET /slow.bin HTTP/1.1\r\nHost: x\r\n\r\n")
            deadline = time.monotonic() + 5
            while not entered.exists() and time.monotonic() < deadline: time.sleep(.01)
            assert entered.exists()
            b = connect(); sockets.append(b); b.sendall(GET)
            time.sleep(.1)  # a worker is gated; b occupies the single queue slot.
            # A full queue must not turn an empty EOF into a 503 job.
            with connect() as empty:
                empty.shutdown(socket.SHUT_WR)
                assert empty.recv(1) == b""
            c = connect(); sockets.append(c); c.sendall(GET)
            status, _, _, header = response(c)
            assert status == 503 and b"Connection: close" in header
            assert c.recv(1) == b""
            server.request_stop()
            gate.unlink()
            assert response(a)[:2] == (200, file_data)
            assert response(b)[0] == 200
            assert a.recv(1) == b"" and b.recv(1) == b""
        finally:
            gate.unlink(missing_ok=True)
            for s in sockets: s.close()
            server.stop()

        # Reject invalid CLI values before opening sockets.
        for args in (["--worker", "0"], ["--queue", "-1"], ["--max", "65537"],
                     ["--buffer", "18446744073709551615"], ["--unknown", "1"]):
            assert subprocess.run([str(BINARY), *args], stderr=subprocess.DEVNULL).returncode != 0
        print("epoll main: concurrency/pipeline/GET discard/half-close/100 requests/deadlines/upload/capacity/accept cap/503/drain/CLI passed")


if __name__ == "__main__":
    main()
