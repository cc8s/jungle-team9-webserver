#!/usr/bin/env python3
"""기능 테스트·데모용 백엔드. 사용법: backend.py <포트>

응답 본문에 자기 포트와 받은 요청(메서드·경로·Connection·X-Forwarded-For·본문)을 적어 돌려준다.
  /api/wait...  1초 뒤에 응답 (느린 백엔드가 worker 를 붙잡는지 보는 비교 테스트 C10 용)
  /api/slow...  5초 뒤에 응답 (프록시 타임아웃 → 504 확인용)
  /api/cut...   응답 도중 연결을 끊는다 (길이는 1000 이라 해 놓고 7바이트만)
"""
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1])


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def reply(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n).decode() if n else ""
        if self.path.startswith("/api/slow"):
            time.sleep(5)
        if self.path.startswith("/api/wait"):
            time.sleep(1)
        if self.path.startswith("/api/cut"):
            self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\npartial")
            self.wfile.flush()
            self.close_connection = True
            return
        text = (
            f"backend={PORT}\nmethod={self.command}\npath={self.path}\n"
            f"connection={self.headers.get('Connection')}\n"
            f"xff={self.headers.get('X-Forwarded-For')}\nbody={body}\n"
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(text)))
        self.end_headers()
        self.wfile.write(text)

    do_GET = do_POST = do_PUT = do_DELETE = reply

    def handle(self):
        # 프록시가 타임아웃으로 먼저 끊은 연결에 쓰면 BrokenPipe. 테스트가 일부러 만드는 상황이라 조용히 넘긴다
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *args):  # 조용히
        pass


class Server(ThreadingHTTPServer):
    request_queue_size = 1024  # 기본 5: 동시 접속이 많으면 SYN 이 버려져 서버가 아니라 이 백엔드가 먼저 병목이 된다
    daemon_threads = True


Server(("127.0.0.1", PORT), Handler).serve_forever()
