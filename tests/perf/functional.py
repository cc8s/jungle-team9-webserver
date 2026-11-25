#!/usr/bin/env python3
"""Run wrk response checks plus isolated slow-client probes against a supplied server.

The caller owns the server and must supply a dedicated fixture root. This runner
never starts, stops, or reconfigures another server. F06/F07 require the existing
isolated actual-main integration suite (tests/tools/test_epoll_server.py).
"""
import argparse
import contextlib
import json
from pathlib import Path
import socket
import struct
import sys
import subprocess
import tempfile
import time
from urllib.parse import urlsplit

HERE = Path(__file__).resolve().parent


def parse_validation(output):
    lines = [line.removeprefix("PERF_VALIDATE ") for line in output.splitlines()
             if line.startswith("PERF_VALIDATE ")]
    if len(lines) != 1:
        raise AssertionError("wrk did not report exactly one PERF_VALIDATE result")
    result = json.loads(lines[0])
    if result["checked"] <= 0 or result["mismatches"]:
        raise AssertionError(f"response validation failed: {result}")
    for field in ("connect_errors", "read_errors", "write_errors", "timeout_errors"):
        if result[field]:
            raise AssertionError(f"unexpected wrk {field}: {result}")
    # wrk can count an incomplete response at the end differently; require every
    # response it reported complete to have gone through the response callback.
    if result["checked"] != result["requests"]:
        raise AssertionError(f"completed/validated response count differs: {result}")
    return result


def open_socket(address):
    connection = socket.create_connection(address, timeout=3)
    connection.settimeout(8)
    return connection


def receive_response(connection):
    data = b""
    while b"\r\n\r\n" not in data:
        part = connection.recv(65536)
        if not part:
            raise AssertionError("EOF before response headers")
        data += part
    header, body = data.split(b"\r\n\r\n", 1)
    fields = {}
    for line in header.split(b"\r\n")[1:]:
        key, value = line.split(b":", 1)
        fields[key.lower()] = value.strip()
    length = int(fields[b"content-length"])
    while len(body) < length:
        part = connection.recv(min(65536, length - len(body)))
        if not part:
            raise AssertionError("EOF before response body")
        body += part
    return int(header.split()[1]), fields, body


class Runner:
    def __init__(self, args):
        self.args = args
        parsed = urlsplit(args.base_url)
        if parsed.scheme != "http" or not parsed.hostname or parsed.path not in ("", "/"):
            raise ValueError("--base-url must be a plain http://host:port server origin")
        self.address = (parsed.hostname, parsed.port or 80)
        self.base = args.base_url.rstrip("/")
        self.expected = (args.fixture_root / "www/perf/small.bin").read_bytes()
        self.output = args.output
        self.output.mkdir(parents=True, exist_ok=True)
        self.results = []

    def request(self, connection, path="/perf/small.bin", close=False):
        authority = urlsplit(self.base).netloc
        connection.sendall((f"GET {path} HTTP/1.1\r\nHost: {authority}\r\n" +
                            ("Connection: close\r\n" if close else "") + "\r\n").encode())

    def probe(self):
        start = time.monotonic()
        with open_socket(self.address) as connection:
            self.request(connection, close=True)
            status, _, body = receive_response(connection)
        assert status == 200 and body == self.expected
        return time.monotonic() - start

    def wrk(self, label, connections, threads=1, identity=False, duration=None):
        duration = self.args.duration if duration is None else duration
        command = [self.args.wrk, f"-t{threads}", f"-c{connections}",
                   f"-d{duration}s", "--timeout", "15s", "-s", str(HERE / "validate.lua"),
                   self.base + "/perf/small.bin", "--",
                   str(self.args.fixture_root / "www/perf/small.bin")]
        if identity:
            command.append("identity")
        started = time.monotonic()
        completed = subprocess.run(command, text=True, capture_output=True,
                                   timeout=duration + 30)
        (self.output / f"{label}.log").write_text(completed.stdout + completed.stderr)
        assert completed.returncode == 0, f"wrk exited {completed.returncode}: {label}"
        result = parse_validation(completed.stdout)
        result.update(label=label, command=command, wall_seconds=time.monotonic() - started)
        self.results.append(result)
        return result

    def slow_headers(self):
        with contextlib.ExitStack() as stack:
            sockets = [stack.enter_context(open_socket(self.address)) for _ in range(8)]
            for connection in sockets:
                connection.sendall(b"GET /perf/small.bin HTTP/1.1\r\nHo")
            # Prove normal work completes before the held headers' 5s deadline.
            delay = self.probe()
            assert delay < 3, f"slow headers stalled normal probe ({delay:.3f}s)"
            self.wrk("F03-slow-headers", 16)
            for connection in sockets:
                status, _, _ = receive_response(connection)
                assert status == 408
            self.results.append({"label": "F03-probe", "probe_seconds": delay})

    def idle(self):
        with contextlib.ExitStack() as stack:
            sockets = [stack.enter_context(open_socket(self.address)) for _ in range(8)]
            for connection in sockets:
                self.request(connection)
                assert receive_response(connection)[2] == self.expected
            delay = self.probe()
            assert delay < 3, f"idle connections stalled normal probe ({delay:.3f}s)"
            self.wrk("F04-idle", 16)
            time.sleep(max(0, 5.3 - self.args.duration))
            for connection in sockets:
                assert connection.recv(1) == b"", "idle connection did not expire"

    def nonreader(self):
        with open_socket(self.address) as connection:
            connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            self.request(connection, "/perf/large.bin", close=True)
            time.sleep(0.1)
            delay = self.probe()
            assert delay < 3, f"non-reader stalled normal probe ({delay:.3f}s)"
            self.wrk("F05-nonreader", 16)
            # Draining changes the workload. No assertion about exact timeout
            # is made here; existing actual-TCP/no-drain tests cover that rule.
        self.results.append({"label": "F05-probe", "probe_seconds": delay,
                             "timeout_scope": "other-request progress; no exact sender expiry claim"})

    def disconnect(self):
        command = [self.args.wrk, "-t1", "-c16", f"-d{self.args.duration}s", "--timeout", "15s",
                   "-s", str(HERE / "validate.lua"), self.base + "/perf/small.bin", "--",
                   str(self.args.fixture_root / "www/perf/small.bin")]
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as worker:
            try:
                for _ in range(16):
                    connection = open_socket(self.address)
                    connection.sendall(b"GET /perf/small.bin HTTP/1.1\r\nHo")
                    connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                    connection.close()
                stdout, stderr = worker.communicate(timeout=self.args.duration+30)
                (self.output / "F08-disconnect.log").write_text(stdout+stderr)
                assert worker.returncode == 0
                result = parse_validation(stdout)
                result["label"] = "F08-disconnect"
                self.results.append(result)
            except BaseException:
                worker.kill()
                worker.wait()
                raise
        self.results.append({"label": "F08-recovery", "probe_seconds": self.probe()})

    def keepalive_boundary(self):
        with open_socket(self.address) as connection:
            for count in range(1, 101):
                self.request(connection)
                status, headers, body = receive_response(connection)
                assert status == 200 and body == self.expected
                if count == 100:
                    assert headers.get(b"connection", b"").lower() == b"close"
            assert connection.recv(1) == b"", "100th response did not close the connection"
        self.results.append({"label": "F09-explicit-100", "requests": 100})
        result = self.wrk("F09-reconnect", 1, duration=max(self.args.duration, 10))
        assert result["checked"] > 100, "need more than 100 responses to exercise wrk reconnect"

    def run(self):
        selected = set(self.args.cases.split(","))
        if "F01" in selected:
            for connections in self.args.connections:
                self.wrk(f"F01-c{connections}", connections)
        if "F02" in selected:
            self.wrk("F02-identity", 16, threads=4, identity=True)
        if "F03" in selected:
            self.slow_headers()
        if "F04" in selected:
            self.idle()
        if "F05" in selected:
            self.nonreader()
        if "F08" in selected:
            self.disconnect()
        if "F09" in selected:
            self.keepalive_boundary()
        if selected & {"F06", "F07"}:
            script = HERE.parent / "tools/test_epoll_server.py"
            program = ("import importlib.util, pathlib, sys; "
                       "spec=importlib.util.spec_from_file_location('epoll_suite', sys.argv[1]); "
                       "module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module); "
                       "module.BINARY=pathlib.Path(sys.argv[2]); module.main()")
            completed = subprocess.run([sys.executable, "-c", program, str(script),
                                        str(self.args.integration_binary.resolve())],
                                       capture_output=True, text=True, timeout=180)
            (self.output / "F06-F07-existing-integration.log").write_text(completed.stdout+completed.stderr)
            assert completed.returncode == 0, "existing isolated main integration suite failed"
            for label in ("F06", "F07"):
                if label in selected:
                    self.results.append({"label": label, "status": "passed-existing-integration",
                                         "scope": "explicit sockets, separate local server and deterministic queue gate; not wrk"})
        self.results.append({"label": "final-probe", "probe_seconds": self.probe()})
        (self.output / "functional.json").write_text(json.dumps(self.results, indent=2)+"\n")
        (self.output / "failure.json").unlink(missing_ok=True)
        return self.results


def self_test():
    from fixtures import create
    with tempfile.TemporaryDirectory(prefix="perf-functional-selftest-") as directory:
        root = Path(directory)
        manifest = create(root)
        assert manifest["small.bin"]["bytes"] == 1024
        assert manifest["medium.bin"]["bytes"] == 65536
        assert manifest["large.bin"]["bytes"] == 16777216
        assert (root / "www/perf/client-1.bin").read_bytes() != (root / "www/perf/client-2.bin").read_bytes()
        assert create(root) == manifest
    good = {"checked": 10, "mismatches": 0, "requests": 10, "duration_us": 1000000,
            "connect_errors": 0, "read_errors": 0, "write_errors": 0, "timeout_errors": 0}
    assert parse_validation("PERF_VALIDATE " + json.dumps(good)) == good
    for key in ("mismatches", "connect_errors", "read_errors", "write_errors", "timeout_errors"):
        bad = dict(good, **{key: 1})
        try:
            parse_validation("PERF_VALIDATE " + json.dumps(bad))
        except AssertionError:
            pass
        else:
            raise AssertionError(f"failure was accepted: {key}")
    print("functional self-test: fixture hashes/sizes and nonzero-error rejection passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--base-url")
    parser.add_argument("--fixture-root", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--wrk", default="wrk")
    parser.add_argument("--integration-binary", type=Path,
                        help="Linux product executable for isolated F06/F07 existing-suite replay")
    parser.add_argument("--duration", type=int, default=10)
    parser.add_argument("--connections", default="1,2,4,8,16,32,64")
    parser.add_argument("--cases", default="F01,F02,F03,F04,F05,F08,F09")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not all((args.base_url, args.fixture_root, args.output)):
        parser.error("--base-url, --fixture-root and --output are required")
    if args.duration < 1:
        parser.error("--duration must be positive")
    try:
        args.connections = [int(value) for value in args.connections.split(",")]
        if not args.connections or any(value < 1 for value in args.connections):
            raise ValueError()
    except ValueError:
        parser.error("--connections must be positive comma-separated integers")
    invalid = set(args.cases.split(",")) - {f"F{i:02d}" for i in range(1, 10)}
    if invalid:
        parser.error(f"unknown cases: {sorted(invalid)}")
    if set(args.cases.split(",")) & {"F06", "F07"} and not args.integration_binary:
        parser.error("F06/F07 require --integration-binary; suite starts separate local :8000 server")
    try:
        result = Runner(args).run()
    except (AssertionError, OSError, ValueError, subprocess.SubprocessError) as error:
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / "failure.json").write_text(json.dumps({"error": str(error)})+"\n")
        parser.exit(1, f"functional test failed: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
