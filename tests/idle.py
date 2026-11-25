#!/usr/bin/env python3
"""노는 연결 만들기 (비교 테스트 C4·C6 용). 사용법: idle.py <포트> <개수> <유지 초>

연결만 맺고 아무것도 보내지 않은 채 유지한다. 서버가 느린 손님(Slowloris)에 얼마나 강한지 본다.
"""
import socket
import sys
import time

port, count, hold = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
socks = []
for _ in range(count):
    s = socket.socket()
    s.settimeout(2)
    try:
        s.connect(("127.0.0.1", port))
        socks.append(s)
    except OSError:
        break
print(f"idle connections open: {len(socks)}", flush=True)
time.sleep(hold)
