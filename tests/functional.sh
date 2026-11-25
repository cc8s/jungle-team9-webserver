#!/bin/bash
# 기능·엣지 케이스 통합 테스트 (이슈 #11): 실제 서버를 띄우고 curl·nc 로 확인한다.
#   사용법: bash tests/functional.sh [서버 실행 파일 (기본 ./webserver)]
# 필요: curl, nc (openbsd), python3. 서버·백엔드는 임시 포트와 임시 폴더에 띄우고 끝나면 정리한다.
# D1 의 비교 기준: ⑤ prefork 와 ⑥ epoll 이 같은 이 테스트를 통과해야 한다 (C1).

BIN=${1:-./webserver}
cd "$(dirname "$0")/.." || exit 1
[ -x "$BIN" ] || { echo "no server binary: $BIN (run make first)"; exit 1; }

P=$((20000 + RANDOM % 20000)) # 서버. 백엔드는 P+1, P+2
B1=$((P + 1)); B2=$((P + 2))
U=http://127.0.0.1:$P
ROOT=$(mktemp -d)
PIDS=()
pass=0; fail=0

cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$ROOT"; }
trap cleanup EXIT

check() { # 이름 기대 실제
  if [ "$2" = "$3" ]; then pass=$((pass + 1)); echo "  ok   $1"
  else fail=$((fail + 1)); echo "  FAIL $1 (기대 '$2', 실제 '$3')"; fi
}
code() { curl -s -o /dev/null -w '%{http_code}' --max-time 10 "$@"; }
raw()  { printf "$1" | timeout 5 nc -q 1 127.0.0.1 "$P" | head -1 | tr -d '\r'; } # 응답 첫 줄
wait_listen() { for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$1) 2>/dev/null && return 0; sleep 0.1; done; return 1; }

# ---- 준비물 ----
mkdir -p "$ROOT/css" "$ROOT/empty"
echo '<h1>안녕</h1>' > "$ROOT/index.html"
echo 'h1{color:red}' > "$ROOT/css/site.css"
head -c 3000000 /dev/urandom > "$ROOT/big.bin"
echo SECRET > "$ROOT/../functional-secret-$$.txt"
python3 tests/backend.py $B1 & PIDS+=($!)
python3 tests/backend.py $B2 & PIDS+=($!)
# worker 1개: RR 카운터가 한 곳에 모여 정확히 번갈아 감 (여러 개면 worker 마다 따로 돈다, D4)
"$BIN" -p $P -r "$ROOT" -w 1 -t 2 -R /api=127.0.0.1:$B1,127.0.0.1:$B2 > "$ROOT/../functional-server-$$.log" 2>&1 & SRV=$!
PIDS+=($SRV)
wait_listen $P && wait_listen $B1 && wait_listen $B2 || { echo "서버·백엔드가 안 뜸"; exit 1; }

echo "== 정적 파일"
check "GET / → index.html"          "<h1>안녕</h1>" "$(curl -s $U/)"
check "CSS 의 Content-Type"          "text/css; charset=utf-8" "$(curl -s -o /dev/null -w '%{content_type}' $U/css/site.css)"
check "없는 파일 404"                 404 "$(code $U/nope.html)"
check "HEAD 는 헤더만"                "200 0" "$(curl -s -I -o /dev/null -w '%{http_code} %{size_download}' $U/)"
curl -s $U/big.bin -o "$ROOT/got.bin"
check "3MB 파일이 그대로"             same "$(cmp -s "$ROOT/got.bin" "$ROOT/big.bin" && echo same || echo differ)"
check "index 없는 디렉터리 403"       403 "$(code $U/empty/)"

echo "== 경로 탈출 (D6)"
check "/../ 403"                      403 "$(code --path-as-is $U/../functional-secret-$$.txt)"
check "%2e%2e 403"                    403 "$(code --path-as-is $U/%2e%2e/functional-secret-$$.txt)"
check "..%2f 403"                     403 "$(code --path-as-is "$U/..%2ffunctional-secret-$$.txt")"
check "루트 밖은 파일이 없어도 403 (존재 여부를 안 알림)" 403 "$(code --path-as-is $U/../functional-no-such-file)"
check "%00 400"                       400 "$(code "$U/index.html%00.png")"
ln -s "../functional-secret-$$.txt" "$ROOT/leak"
check "루트 밖 심볼릭 링크 403"        403 "$(code $U/leak)"

echo "== 메서드·버전·형식 오류"
check "POST 정적 405"                 405 "$(code -X POST $U/)"
check "405 에 Allow"                  "Allow: GET, HEAD" "$(curl -s -i -X POST $U/ | grep -i '^Allow' | tr -d '\r')"
check "모르는 메서드 501"              501 "$(code -X FOO $U/)"
check "Transfer-Encoding 501"         501 "$(code -H 'Transfer-Encoding: chunked' --data-binary x $U/)"
check "Host 없는 1.1 → 400"           "HTTP/1.1 400 Bad Request" "$(raw 'GET / HTTP/1.1\r\n\r\n')"
check "HTTP/2.0 → 505"                "HTTP/1.1 505 HTTP Version Not Supported" "$(raw 'GET / HTTP/2.0\r\nHost: a\r\n\r\n')"
check "요청 라인 깨짐 → 400"           "HTTP/1.1 400 Bad Request" "$(raw 'GARBAGE\r\n\r\n')"
check "3000자 URI → 414"              414 "$(code "$U/$(head -c 3000 /dev/zero | tr '\0' a)")"
check "2MB 본문 → 413"                413 "$(head -c 2000000 /dev/zero | code --data-binary @- $U/up)"
check "오류 본문에 요청 경로 없음 (D7)" 0 "$(printf 'GET /<script>x HTTP/2.0\r\nHost: a\r\n\r\n' | nc -q 1 127.0.0.1 $P | grep -c script)"
check "응답이 Connection: close"       "Connection: close" "$(curl -s -i $U/ | grep -i '^Connection' | tr -d '\r')"

echo "== 리버스 프록시 (D2·D4)"
check "정적과 구분: /apix 는 정적"      404 "$(code $U/apix/hello)"
check "대소문자: /API 는 정적"          404 "$(code $U/API/users)"
r=$(curl -s -H 'Connection: keep-alive' -H 'X-Forwarded-For: 6.6.6.6' "$U/api/users?id=3")
check "접두어를 붙인 채 전달"           "path=/api/users?id=3" "$(echo "$r" | grep ^path=)"
check "백엔드에는 Connection: close"    "connection=close" "$(echo "$r" | grep ^connection=)"
check "XFF 는 우리가 본 주소 (위조 버림)" "xff=127.0.0.1" "$(echo "$r" | grep ^xff=)"
check "POST 본문 전달"                 "body=name=jun&x=1" "$(curl -s -X POST -d 'name=jun&x=1' $U/api/f | grep ^body=)"
check "DELETE 그대로"                  "method=DELETE" "$(curl -s -X DELETE $U/api/i/7 | grep ^method=)"
check "프록시 경로 .. 403"              403 "$(code --path-as-is $U/api/../admin)"
check "Round-Robin 10번 → 5:5"          "5 5" "$(for _ in $(seq 10); do curl -s $U/api/x | grep ^backend=; done | sort | uniq -c | awk '{printf "%s ", $1}' | sed 's/ $//')"
t0=$(date +%s%N); c=$(code $U/api/slow); ms=$(( ($(date +%s%N) - t0) / 1000000 ))
check "느린 백엔드 504"                 504 "$c"
check "504 는 -t(2초) 근처에서"          yes "$([ $ms -ge 1800 ] && [ $ms -lt 4000 ] && echo yes || echo "no(${ms}ms)")"
check "응답 도중 끊는 백엔드 → 서버 생존" 200 "$(curl -s -o /dev/null --max-time 5 $U/api/cut; code $U/)"

echo "== 갑작스러운 연결 종료·느린 손님"
printf 'GET / HTTP/1.1\r\nHo' | timeout 1 nc -q 0 127.0.0.1 $P > /dev/null
check "헤더 도중 끊어도 서버 생존"       200 "$(code $U/)"
t0=$(date +%s%N)
{ for ch in G E T ' ' / ' ' H T T P; do printf "$ch"; sleep 0.5; done; } | nc 127.0.0.1 $P > /dev/null
ms=$(( ($(date +%s%N) - t0) / 1000000 ))
check "찔끔 보내기는 -t(2초)에 끊김"     yes "$([ $ms -lt 4500 ] && echo yes || echo "no(${ms}ms)")"
for _ in 1 2 3; do curl -s -o /dev/null --max-time 0.2 --limit-rate 100k $U/big.bin; done
check "받다가 끊는 손님 (SIGPIPE) 뒤에도 서버 생존" 200 "$(code $U/)"

echo "== 동시 접속"
seq 1000 | xargs -P 40 -I{} curl -s -o /dev/null -w '%{http_code}\n' --max-time 10 $U/ > "$ROOT/codes.txt"
check "동시 40 × 1000 요청 전부 200"     "1000 200" "$(sort "$ROOT/codes.txt" | uniq -c | awk '{printf "%s %s", $1, $2}')"

echo "== 종료"
kill -TERM $SRV
for _ in $(seq 30); do kill -0 $SRV 2>/dev/null || break; sleep 0.1; done
kill -0 $SRV 2>/dev/null && alive=alive || alive=gone
check "SIGTERM 로 종료"                 gone "$alive"
wait $SRV 2>/dev/null; check "종료 코드 0" 0 $?
rm -f "$ROOT/../functional-secret-$$.txt" "$ROOT/../functional-server-$$.log"

echo
echo "통과 $pass, 실패 $fail"
[ "$fail" -eq 0 ]
