#!/bin/bash
# 동시성 모델 비교 테스트 (설계서 4-1절 C2~C6, C8, C9). 같은 명령으로 ⑤ prefork 와 ⑥ epoll 을 나란히 잰다.
#   사용법: bash tests/compare.sh [서버 실행 파일 (기본 ./webserver)] [worker 수 (기본 4)]
# 필요: wrk, curl, python3. 결과는 화면에 그대로 찍는다 (원시 출력을 저장해 README 표로 옮긴다).
# C1(기능 회귀)은 tests/functional.sh, C7(누수)은 make asan + functional.sh 를 ASan 빌드로.

BIN=${1:-./webserver}
N=${2:-4}
DUR=${DUR:-5}   # wrk 한 번에 몇 초
TMO=${TMO:-2}   # 서버 -t
cd "$(dirname "$0")/.." || exit 1
[ -x "$BIN" ] || { echo "no server binary: $BIN"; exit 1; }
command -v wrk > /dev/null || { echo "wrk 가 필요합니다 (apt install wrk)"; exit 1; }

P=$((20000 + RANDOM % 20000)); U=http://127.0.0.1:$P
ROOT=$(mktemp -d); echo '<h1>hello</h1>' > "$ROOT/index.html"
SRV=
cleanup() { [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; wait 2>/dev/null; rm -rf "$ROOT"; }
trap cleanup EXIT

start() {
  "$BIN" -p $P -r "$ROOT" -w $N -t $TMO > "$ROOT/server.log" 2>&1 & SRV=$!
  for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$P) 2>/dev/null && return 0; sleep 0.1; done
  echo "서버가 안 뜸"; exit 1
}
stop() { kill -TERM "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; echo "  서버 종료 코드: $?"; SRV=; }
rss_kb() { # 서버(부모)와 자식들의 RSS 합 (KB)
  { echo "$SRV"; pgrep -P "$SRV"; } | xargs -I{} ps -o rss= -p {} 2>/dev/null | awk '{s+=$1} END {print s+0}'
}
wrk_line() { grep -E "Requests/sec|Latency|Socket errors|Non-2xx|requests in" ; }

echo "환경: 코어 $(nproc)개, worker $N, 서버 -t ${TMO}s, wrk ${DUR}초, 바이너리 $BIN"

echo; echo "== C2 동시 접속 ≤ N (wrk -c $N)"
start; wrk -t2 -c $N -d${DUR}s $U/ | wrk_line; stop

echo; echo "== C3 동시 접속 ≫ N (wrk -c 1000)"
start; wrk -t4 -c 1000 -d${DUR}s --latency $U/ | grep -E "Requests/sec|Latency|50%|99%|Socket errors|Non-2xx"; stop

echo; echo "== C4 노는 연결 1000개를 연 채로 curl 한 번 (최대 10초 기다림)"
start
python3 tests/idle.py $P 1000 14 > "$ROOT/idle.log" &
IDLE=$!; sleep 1.5
cat "$ROOT/idle.log"
echo "  curl: $(curl -s -o /dev/null -w 'HTTP %{http_code}, %{time_total}초' --max-time 10 $U/)  (HTTP 000 = 10초 안에 응답 없음)"
echo "== C6 같은 순간의 메모리 (서버 + 자식 RSS 합)"
echo "  RSS 합: $(rss_kb) KB"
kill $IDLE 2>/dev/null; wait $IDLE 2>/dev/null; stop

echo; echo "== C5 크래시 주입: 부하 중 worker 하나를 kill -9 (wrk -c 200)"
start
wrk -t2 -c 200 -d$((DUR + 3))s $U/ > "$ROOT/c5.log" &
W=$!; sleep 2
V=$(pgrep -P $SRV | head -1); echo "  worker $V 를 kill -9"; kill -9 "$V"
wait $W; wrk_line < "$ROOT/c5.log"
echo "  이후 요청: $(curl -s -o /dev/null -w '%{http_code}' --max-time 5 $U/)"; stop

echo; echo "== C8 셧다운: 부하 중 SIGTERM"
start
wrk -t2 -c 100 -d$((DUR + 3))s $U/ > /dev/null 2>&1 &
W=$!; sleep 2
kill -TERM $SRV; wait $SRV 2>/dev/null; echo "  서버 종료 코드: $?"; SRV=
sleep 0.3; echo "  남은 서버 프로세스: $(pgrep -f "webserver -p $P" | wc -l)"
kill $W 2>/dev/null; wait $W 2>/dev/null

echo; echo "== C9 keep-alive: wrk 기본(연결 유지 요청) vs Connection: close (wrk -c 100)"
start
echo "  [연결 유지를 요청]"; wrk -t2 -c 100 -d${DUR}s $U/ | wrk_line
echo "  [Connection: close]"; wrk -t2 -c 100 -d${DUR}s -H 'Connection: close' $U/ | wrk_line
echo "  서버 로그의 연결 수 (⑤ 는 요청마다 연결 → 요청 수와 같음): $(grep -c connected "$ROOT/server.log")"
stop
