#!/bin/bash
# T-LB (설계서 4-2): L7 1단(S1) 대 L4 → L7 2단(S2). D10 의 L4 중계 모드(-L) 를 시험하고 잰다.
#   사용법: bash tests/lb.sh [서버 실행 파일 (기본 ./webserver)]
#   환경변수: W(전체 worker 수, 기본 4)  DUR(wrk 한 번에 몇 초, 기본 5)  ROUNDS(기본 3)
# 필요: wrk, curl, python3, setsid, taskset(코어 고정 시험은 코어가 7개 이상일 때만)
#
# 구성 (같은 PC, 포트만 다름):  S1: 손님 → L7(S1)                 → 백엔드 둘
#                              S2: 손님 → L4 → L7(A) / L7(B)       → 백엔드 둘   (A·B 는 각자 자기 id.txt 를 가짐)
# 전체 worker 수를 같게 맞춘다: S1 은 W, S2 는 A·B 가 각 W/2 (+ L4 worker 1).
# 가용성은 "요청을 일정 속도(초당 25)로 보내는" 방식으로 잰다: 서버가 느려져도 보내는 속도가 안 변해서 성공률이 정직하다.

BIN=${1:-./webserver}
W=${W:-4}
DUR=${DUR:-5}
ROUNDS=${ROUNDS:-3}
cd "$(dirname "$0")/.." || exit 1
[ -x "$BIN" ] || { echo "no server binary: $BIN"; exit 1; }
for t in wrk curl python3 setsid; do command -v $t > /dev/null || { echo "$t 가 필요합니다"; exit 1; }; done

in_use() { (exec 3<>/dev/tcp/127.0.0.1/$1) 2>/dev/null; }
while :; do # 포트 6개 (임시 포트 범위 32768~ 아래에서)
  B=$((20000 + RANDOM % 12000))
  ok=1; for i in 0 1 2 3 4 5; do in_use $((B + i)) && ok=0; done
  [ $ok = 1 ] && break
done
S1=$B; PA=$((B + 1)); PB=$((B + 2)); L4=$((B + 3)); BE1=$((B + 4)); BE2=$((B + 5))
ROOT=$(mktemp -d); mkdir "$ROOT/a" "$ROOT/b" "$ROOT/s1"
for d in a b s1; do
  echo "$d" > "$ROOT/$d/id.txt"
  echo '<h1>hello</h1>' > "$ROOT/$d/index.html"
  head -c 1024 /dev/zero | tr '\0' x > "$ROOT/$d/small.html"
  head -c 102400 /dev/urandom > "$ROOT/$d/mid.bin"
done
head -c 3000000 /dev/urandom > "$ROOT/s1/big.bin"; cp "$ROOT/s1/big.bin" "$ROOT/a/big.bin"; cp "$ROOT/s1/big.bin" "$ROOT/b/big.bin"
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill -9 -- "-$p" 2> /dev/null; kill -9 "$p" 2> /dev/null; done; wait 2> /dev/null; rm -rf "$ROOT"; }
trap cleanup EXIT

wait_listen() { for _ in $(seq 50); do in_use "$1" && return 0; sleep 0.1; done; return 1; }
spawn() { # 변수이름 코어목록("" = 제한 없음) 명령...   → 자기 프로세스 그룹으로 띄우고 pid 를 그 변수에
  local var=$1 cores=$2; shift 2
  if [ -n "$cores" ]; then setsid taskset -c "$cores" "$@" > "$ROOT/$var.log" 2>&1 & else setsid "$@" > "$ROOT/$var.log" 2>&1 & fi
  local pid=$!; PIDS+=($pid); printf -v "$var" '%s' "$pid"
}
killgrp() { kill -9 -- "-$1" 2> /dev/null; wait "$1" 2> /dev/null; } # worker 까지 (부모만 죽이면 worker 가 listen 소켓을 쥔 채 계속 응답한다)
stopgrp() { kill -STOP -- "-$1"; }
contgrp() { kill -CONT -- "-$1"; }

python3 tests/backend.py $BE1 & PIDS+=($!)
python3 tests/backend.py $BE2 & PIDS+=($!)
wait_listen $BE1 && wait_listen $BE2 || { echo "백엔드가 안 뜸"; exit 1; }

HALF=$((W / 2)); [ $HALF -lt 1 ] && HALF=1
S1_ARGS=(-p $S1 -r "$ROOT/s1" -w $W -t 2 -R /api=127.0.0.1:$BE1,127.0.0.1:$BE2)
start_s1() { spawn P_S1 "$1" "$BIN" "${S1_ARGS[@]}"; wait_listen $S1 || { echo "S1 이 안 뜸"; exit 1; }; }
start_a() { spawn P_A "$1" "$BIN" -p $PA -r "$ROOT/a" -w ${2:-$HALF} -t 2 -R /api=127.0.0.1:$BE1,127.0.0.1:$BE2; wait_listen $PA || { echo "L7(A) 가 안 뜸"; exit 1; }; }
start_b() { spawn P_B "$1" "$BIN" -p $PB -r "$ROOT/b" -w ${2:-$HALF} -t 2 -R /api=127.0.0.1:$BE1,127.0.0.1:$BE2; wait_listen $PB || { echo "L7(B) 가 안 뜸"; exit 1; }; }
start_l4() { spawn P_L4 "$1" "$BIN" -p $L4 -L 127.0.0.1:$PA,127.0.0.1:$PB -w 1 -t 2; wait_listen $L4 || { echo "L4 가 안 뜸"; exit 1; }; }

pass=0; fail=0
check() { if [ "$2" = "$3" ]; then pass=$((pass + 1)); echo "  ok   $1"; else fail=$((fail + 1)); echo "  FAIL $1 (기대 '$2', 실제 '$3')"; fi; }
code() { curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$@"; }
dist() { # URL 을 n 번 요청해 id.txt 별 개수 ("a:5 b:5")
  for _ in $(seq "$2"); do curl -s --max-time 5 "$1" | tr -d '\n'; echo; done | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' | sed 's/ $//'
}

# ============ 1. 기능: L4 가 연결을 나누고 장애를 건너뛰나 ============
echo "== 1. L4 중계 기능 (D10)"
start_a ""; start_b ""; start_l4 ""
# (서버가 떴는지 보려고 연결해 본 것도 L4 의 RR 에 한 번 세어지므로, 첫 요청이 a 인지 b 인지는 정해져 있지 않다)
check "L4 를 거쳐 정적 파일이 옴 (a 또는 b)"  "ok" "$(curl -s --max-time 5 http://127.0.0.1:$L4/id.txt | grep -qE '^(a|b)$' && echo ok || echo no)"
check "연결 10개 → L7 둘에 5:5 (Round-Robin)" "a:5 b:5" "$(dist http://127.0.0.1:$L4/id.txt 10)"
curl -s --max-time 10 http://127.0.0.1:$L4/big.bin -o "$ROOT/got.bin"
check "L4 를 거친 3MB 가 그대로"             same "$(cmp -s "$ROOT/got.bin" "$ROOT/s1/big.bin" && echo same || echo differ)"
check "L4 를 거쳐 프록시 경로(/api)도 동작"   "method=POST" "$(curl -s --max-time 5 -X POST -d 'x=1' http://127.0.0.1:$L4/api/f | grep ^method=)"
check "keep-alive 가 L4 를 통과 (한 연결에 두 요청)" 1 "$(curl -s -o /dev/null -o /dev/null -w '%{num_connects}\n' http://127.0.0.1:$L4/ http://127.0.0.1:$L4/small.html | awk '{s+=$1} END {print s}')"
# 반닫기 전달: L7 이 응답 뒤 연결을 닫으면(Connection: close) L4 도 손님 쪽을 닫아야 한다. cat 은 닫혀야 끝남
t0=$(date +%s%N)
timeout 5 bash -c "exec 3<>/dev/tcp/127.0.0.1/$L4; printf 'GET /id.txt HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n' >&3; cat <&3 > /dev/null"
ms=$(( ($(date +%s%N) - t0) / 1000000 ))
check "L7 이 닫으면 L4 도 손님 연결을 바로 닫음 (${ms}ms)" yes "$([ $ms -lt 1000 ] && echo yes || echo no)"
killgrp "$P_A"
check "L7(A) 가 죽어도 6번 모두 성공, 전부 b"  "b:6" "$(dist http://127.0.0.1:$L4/id.txt 6)"
start_a ""
sleep 0.3
check "L7(A) 를 다시 띄우면 다시 5:5"          "a:5 b:5" "$(dist http://127.0.0.1:$L4/id.txt 10)"
killgrp "$P_A"; killgrp "$P_B"
check "L7 이 모두 죽으면 연결이 닫힘 (HTTP 000)" "000" "$(code http://127.0.0.1:$L4/id.txt)"
start_a ""; start_b ""
check "복구 뒤 다시 정상"                       200 "$(code http://127.0.0.1:$L4/id.txt)"
kill -TERM "$P_L4"; wait "$P_L4" 2> /dev/null; rc=$?
check "L4 에 SIGTERM → 정상 종료 (종료 코드 0, ASan 이면 누수 없음)" 0 "$rc"
start_l4 ""
killgrp "$P_L4"
check "L4 가 죽으면 전부 실패 (SPOF 가 L4 로)"   "000" "$(code http://127.0.0.1:$L4/id.txt)"
killgrp "$P_A"; killgrp "$P_B"
echo "  (1단계 합계: 통과 $pass, 실패 $fail)"
# ONLY_FUNC=1 이면 기능만 보고 끝낸다 (ASan 으로 빌드한 서버를 빠르게 확인할 때)
[ -n "$ONLY_FUNC" ] && [ "$fail" -eq 0 ] && exit 0
[ -n "$ONLY_FUNC" ] && exit 1

# ============ 2. 속도: 같은 자원에서 S1 대 S2 ============
echo; echo "== 2. 속도 — 같은 자원 (전체 worker $W 로 같음): S1 = L7 하나, S2 = L4 → L7 둘 (각 $HALF)"
summ() { awk '/Requests\/sec/ {rps=$2} /99%/ {p99=$2} /Socket errors/ {e=" ["$0"]"} END {printf "%9s req/s, p99 %s%s\n", rps, p99, e}' | sed 's/  */ /g'; }
speed() { # 이름, 주소 경로, wrk 추가 인자...
  local name=$1 path=$2; shift 2
  for r in $(seq "$ROUNDS"); do
    start_s1 ""; printf '  [%d회] S1  %-26s ' "$r" "$name"; wrk -t2 -c 100 -d${DUR}s --latency "$@" http://127.0.0.1:$S1$path | summ; killgrp "$P_S1"
    start_a ""; start_b ""; start_l4 ""
    printf '  [%d회] S2  %-26s ' "$r" "$name"; wrk -t2 -c 100 -d${DUR}s --latency "$@" http://127.0.0.1:$L4$path | summ
    killgrp "$P_L4"; killgrp "$P_A"; killgrp "$P_B"
  done
}
speed "LB-S1 1KB keep-alive" /small.html
speed "LB-S1 1KB close" /small.html -H 'Connection: close'
speed "LB-S3 /api (백엔드 파이썬)" /api/hello

# ============ 3. 속도: 코어를 나눠 "기계 여러 대" 흉내 (LB-S5) ============
NC=$(nproc)
if command -v taskset > /dev/null && [ "$NC" -ge 7 ]; then
  echo; echo "== 3. 속도 — 코어 고정 (LB-S5, 100KB 파일): S1c = L7 하나(코어 0-1), S2c = L4(코어 6) → L7 둘(코어 0-1, 2-3). wrk 는 코어 4-5"
  echo "   (WSL2 의 코어는 가상이라 같은 물리 코어의 하이퍼스레드일 수 있다 → 나눠도 일부 겹칠 수 있음)"
  for r in $(seq "$ROUNDS"); do
    start_s1 "0,1" ; printf '  [%d회] S1c %-26s ' "$r" "100KB"; taskset -c 4,5 wrk -t2 -c 100 -d${DUR}s --latency http://127.0.0.1:$S1/mid.bin | summ; killgrp "$P_S1"
    start_a "0,1" 2; start_b "2,3" 2; start_l4 "6"
    printf '  [%d회] S2c %-26s ' "$r" "100KB"; taskset -c 4,5 wrk -t2 -c 100 -d${DUR}s --latency http://127.0.0.1:$L4/mid.bin | summ
    killgrp "$P_L4"; killgrp "$P_A"; killgrp "$P_B"
  done
else
  echo; echo "== 3. (코어가 7개 미만이거나 taskset 이 없어 코어 고정 시험은 건너뜀)"
fi

# ============ 4. 가용성: 일정 속도로 요청을 보내며 고장을 낸다 ============
# 8초 동안 초당 25건 (총 200건). 3초에 고장. 각 요청은 curl 하나 (--max-time 4). 코드별 개수를 센다
probe() { # URL 고장_함수
  local url=$1 act=$2; : > "$ROOT/codes.txt"
  # 결과는 한 번의 printf 로 한 줄씩 (동시에 쓰는 여러 curl 의 줄이 섞이지 않게)
  ( for _ in $(seq 200); do ( printf '%s\n' "$(code "$url" --max-time 4)" >> "$ROOT/codes.txt" ) & sleep 0.04; done; wait ) &
  local pp=$!; sleep 3; eval "$act"; wait $pp; sleep 0.5
  local ok total; ok=$(grep -c '^200$' "$ROOT/codes.txt"); total=$(grep -c . "$ROOT/codes.txt")
  echo "성공 $ok / $total ($(( 100 * ok / total ))%)  [코드별: $(sort "$ROOT/codes.txt" | uniq -c | awk '{printf "%s×%s ", $2, $1}' | sed 's/ $//')]"
}
echo; echo "== 4. 가용성 (초당 25건 × 8초, 3초 시점에 고장)"
echo "-- LB-A1 L7 하나가 죽음 (kill -9, 프로세스 그룹)"
start_s1 ""; printf '  S1 (L7 하나가 곧 전부)  '; probe http://127.0.0.1:$S1/id.txt 'killgrp "$P_S1"'
start_a ""; start_b ""; start_l4 ""; printf '  S2 (L7(A) 가 죽음)       '; probe http://127.0.0.1:$L4/id.txt 'killgrp "$P_A"'
echo "-- LB-A3 복구: L7(A) 를 다시 띄운 뒤"
start_a ""; sleep 0.3
echo "  요청 10개의 분포: $(dist http://127.0.0.1:$L4/id.txt 10)"
echo "-- LB-A2 L7 하나가 멈춤 (SIGSTOP, 죽지 않음 — 커널은 연결을 계속 받아 줌)"
printf '  S2 (L7(A) 가 멈춤)       '; probe http://127.0.0.1:$L4/id.txt 'stopgrp "$P_A"'
contgrp "$P_A"
echo "-- LB-A4 L4 가 죽음"
printf '  S2 (L4 가 죽음)          '; probe http://127.0.0.1:$L4/id.txt 'killgrp "$P_L4"'
killgrp "$P_A"; killgrp "$P_B"
echo "-- LB-A5 독 요청 (worker 를 죽이는 요청): 서버에 \"테스트 전용 크래시 요청\" 을 넣지 않기로 해서 시험하지 않음 (설계서: 독립적인 고장에만 이중화 효과가 있다는 점은 설명으로)"

echo; echo "1단계(기능) 합계: 통과 $pass, 실패 $fail"
[ "$fail" -eq 0 ]
