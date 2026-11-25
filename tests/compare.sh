#!/bin/bash
# 동시성 모델 비교 테스트 (설계서 4-1절 C2~C6, C8~C10). 같은 바이너리를 -m 으로 바꿔 ⑤ prefork 와 ⑥ epoll 을 잰다.
#   사용법: bash tests/compare.sh [서버 실행 파일 (기본 ./webserver)] [worker 수 (기본 4)]
#   환경변수: ROUNDS(반복, 기본 3)  DUR(wrk 한 번에 몇 초, 기본 5)  TMO(서버 -t, 기본 2)  MODELS(기본 "prefork epoll")
# 필요: wrk, curl, python3.
#
# 같은 실행 안에서 ⑤ ⑥ ⑤ ⑥ … 로 번갈아 여러 번 잰다. WSL 은 시간에 따라 속도가 크게 달라져서 (같은 코드가 몇 분 사이에 4배 차이)
# 따로 잰 숫자끼리는 비교할 수 없고, 번갈아 잰 쌍끼리만 비교할 수 있다.
# C1(기능 회귀)은 tests/functional.sh, C7(누수)은 서버를 ASan 으로 빌드해 functional.sh.

BIN=${1:-./webserver}
N=${2:-4}
ROUNDS=${ROUNDS:-3}
DUR=${DUR:-5}
TMO=${TMO:-2}
MODELS=${MODELS:-"prefork epoll"}
cd "$(dirname "$0")/.." || exit 1
[ -x "$BIN" ] || { echo "no server binary: $BIN"; exit 1; }
command -v wrk > /dev/null || { echo "wrk 가 필요합니다 (apt install wrk)"; exit 1; }

# 포트는 임시(ephemeral) 포트 범위(32768~)보다 낮게: wrk 가 만든 연결이 그 범위를 쓰고 있어 bind 가 가끔 실패한다
P=$((20000 + RANDOM % 12000)); U=http://127.0.0.1:$P
ROOT=$(mktemp -d); echo '<h1>hello</h1>' > "$ROOT/index.html"
SRV=; BE=
cleanup() { [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; [ -n "$BE" ] && kill "$BE" 2>/dev/null; wait 2>/dev/null; rm -rf "$ROOT"; }
trap cleanup EXIT

wait_listen() { for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$1) 2>/dev/null && return 0; sleep 0.1; done; return 1; }
start() { # $1 = 모델
  "$BIN" -p $P -r "$ROOT" -w $N -t $TMO -m "$1" -R /api=127.0.0.1:$((P + 1)) > "$ROOT/server.log" 2>&1 & SRV=$!
  wait_listen $P || { echo "서버가 안 뜸"; exit 1; }
}
stop() { kill -TERM "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; local rc=$?; SRV=; [ $rc -ne 0 ] && echo "  (!!) 서버 종료 코드 $rc"; }
rss_kb() { { echo "$SRV"; pgrep -P "$SRV"; } | xargs -I{} ps -o rss= -p {} 2>/dev/null | awk '{s+=$1} END {print s+0}'; }
summary() { # wrk --latency 출력을 한 줄로
  awk '/^ +Latency +[0-9]/ {lat=$2} /99%/ {p99=$2} /Requests\/sec/ {rps=$2} /Socket errors/ {e=e" ["$0"]"} /Non-2xx/ {e=e" ["$0"]"}
       END {printf "%9s req/s, 평균 %s, p99 %s%s\n", rps, lat, p99, e}' | sed 's/  */ /g'
}

# 백엔드 (C10 용). 모든 모델이 같은 것을 쓴다
python3 tests/backend.py $((P + 1)) & BE=$!
wait_listen $((P + 1)) || { echo "백엔드가 안 뜸"; exit 1; }

echo "환경: 코어 $(nproc)개, worker $N, 서버 -t ${TMO}s, wrk ${DUR}초, ${ROUNDS}회 반복, 모델 순서: $MODELS, 바이너리 $BIN"

repeat() { # 제목, 측정 함수
  echo; echo "== $1"
  for r in $(seq "$ROUNDS"); do
    for m in $MODELS; do
      start "$m"; printf '  [%d회] %-8s ' "$r" "$m"; "$2"; stop
    done
  done
}

m_c2()  { wrk -t2 -c $N -d${DUR}s --latency $U/ | summary; }
m_c3()  { wrk -t4 -c 1000 -d${DUR}s --latency $U/ | summary; }
m_c9()  { wrk -t2 -c 100 -d${DUR}s --latency $U/ | summary; printf '%19s' 'close  '; wrk -t2 -c 100 -d${DUR}s --latency -H 'Connection: close' $U/ | summary; }
m_c10() { wrk -t2 -c 200 -d${DUR}s --timeout 5s --latency $U/api/wait | summary; }

repeat "C2 동시 접속 ≤ worker 수 (wrk -c $N)" m_c2
repeat "C3 동시 접속 ≫ worker 수 (wrk -c 1000)" m_c3
repeat "C9 keep-alive: 첫 줄 = wrk 기본(연결 유지 요청), 둘째 줄 = Connection: close (wrk -c 100)" m_c9
repeat "C10 느린 백엔드: 백엔드가 1초 걸리는 경로 (wrk -c 200)" m_c10

echo; echo "== C4 노는 연결 1000개를 연 채로 curl 한 번 (최대 10초) + C6 같은 순간의 메모리 (서버 + 자식 RSS 합)"
for m in $MODELS; do
  start "$m"; python3 tests/idle.py $P 1000 14 > "$ROOT/idle.log" & IDLE=$!; sleep 1.5
  printf '  %-8s %s | curl: %s | RSS 합 %s KB\n' "$m" "$(cat "$ROOT/idle.log")" \
    "$(curl -s -o /dev/null -w 'HTTP %{http_code}, %{time_total}초' --max-time 10 $U/)" "$(rss_kb)"
  kill $IDLE 2>/dev/null; wait $IDLE 2>/dev/null; stop
done
echo "  (HTTP 000 = 10초 안에 응답 없음)"

echo; echo "== C5 크래시 주입: 부하 중 worker 하나를 kill -9 (wrk -c 200)"
for m in $MODELS; do
  start "$m"; wrk -t2 -c 200 -d$((DUR + 3))s --latency $U/ > "$ROOT/c5.log" & W=$!; sleep 2
  V=$(pgrep -P $SRV | head -1); kill -9 "$V"; wait $W
  printf '  %-8s ' "$m"; summary < "$ROOT/c5.log"
  echo "           직후 요청: $(curl -s -o /dev/null -w '%{http_code}' --max-time 5 $U/)"; stop
done

echo; echo "== C8 셧다운: 부하 중 SIGTERM (wrk -c 100)"
for m in $MODELS; do
  start "$m"; wrk -t2 -c 100 -d$((DUR + 3))s $U/ > /dev/null 2>&1 & W=$!; sleep 2
  kill -TERM $SRV; wait $SRV 2>/dev/null; rc=$?; SRV=; sleep 0.3
  printf '  %-8s 종료 코드 %s, 남은 서버 프로세스 %s\n' "$m" "$rc" "$(pgrep -f "webserver -p $P" | wc -l)"
  kill $W 2>/dev/null; wait $W 2>/dev/null
done
