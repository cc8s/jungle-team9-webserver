# wrk 기능 테스트 도구와 핵심 검증

2026-11-25. 제품 코드·정책을 바꾸지 않고 `docs/perf_test.md`의 기능 검증 도구를 구현했다. 기능별 실행 시간은 1초로 축약했고, 연결 100요청 이후 재연결 검증만 최소 10초로 실행했다. 이 결과는 처리량 기준선이나 정식 10초×전체 기능 계획 완료라는 뜻은 아니다.

## 생성한 도구와 흐름

- `tests/perf/fixtures.py --root ROOT [--clients 8]`: 전용 실행 루트의 `www/perf`에 1KiB/64KiB/16MiB 바이너리와 서로 다른 client-N.bin(1KiB), 바이트 수·SHA-256 manifest를 생성한다. 일반 바이너리는 offset % 256 패턴이다. `uploads_storage`도 전용 루트 아래 준비한다.
- `validate.lua`: wrk 각 스레드에서 기대 fixture를 읽고 모든 완료 응답의 200·본문 전체 바이트·Content-Length를 검사한다. `PERF_VALIDATE` JSON 한 줄에 검사 수·불일치·소켓 오류를 합산한다. identity 모드는 스레드마다 별도 fixture 경로를 사용하며, 물리적 연결별 식별이라는 뜻은 아니다.
- `diagnose.lua`: 상태 코드별 수와 전체 요청·오류·p50/p95/p99(µs)를 `PERF_DIAGNOSTIC` JSON으로 출력한다. `response` 콜백이 있으므로 순수 처리량 측정용 스크립트와 구분한다.
- `functional.py`: wrk를 실행하고 로그·JSON을 저장한다. 응답이 없거나 본문/길이/상태 불일치·예상하지 않은 소켓 오류·검사 수 차이가 있으면 종료 코드 1이다. 보조 TCP 클라이언트로 느린 헤더·idle·무수신·강제 종료를 만들고 정상 GET의 진행을 확인한다.
- F06/F07은 `--integration-binary`로 지정한 제품 실행 파일과 기존 `tests/tools/test_epoll_server.py`를 재사용한다. 별도 client 컨테이너의 localhost:8000에서 독립 제품 서버를 띄워 명시적 소켓·워커 읽기 장벽을 검사한다. 이 두 항목의 결정적 검증은 wrk 자체 검증이 아니다. F06/F07 선택 시 실행 파일이 없으면 CLI 오류로 거절한다.

일반 흐름은 fixture 준비 → 격리된 제품 서버 시작 → wrk/보조 연결 → Lua 응답 검사 → Python 오류 판정 → 원본 로그·요약 저장이다. runner는 사용자 서버를 시작·중지하지 않는다. 호출자가 전용 실행 환경을 제공한다.

## 실제 검증 결과

환경은 `jungle-perf:local` Linux 이미지, 서버·발생기 별도 컨테이너와 전용 network였다. 소스는 당시 작업 디렉토리의 미커밋 변경까지 포함해 복사됐으며 실행 당시 HEAD·상태·patch·소스 hash가 `build/perf-results/functional-smoke/`에 저장됐다. 사용자 8000 포트·www·업로드는 사용하지 않았다. 환경 생성·정리 도구로 본인 functional-smoke 자원만 정리했다.

| 항목 | 실행·입력 | 실제 결과 |
|---|---|---|
| Python self-test | fixture 크기/재생성 hash/클라이언트 식별, JSON 결과 오류 거절 | 통과 |
| F01 | C=1/2/4/8/16/32/64, 각각 1초 | 200·전체 본문·길이 일치, 예상하지 않은 오류 0 |
| F02 | wrk 4스레드·16연결, 스레드별 서로 다른 1KiB fixture | 384개 응답 대응 검증 통과 |
| F03 | 부분 헤더 8개 + wrk C16 1초 | 정상 probe가 헤더 만료 전 완료, wrk 384개 정상; 부분 헤더는 각각 408 |
| F04 | keep-alive idle 8개 + wrk C16 1초 | 정상 probe 진행, wrk 389개 정상; idle EOF 확인 |
| F05 | 16MiB를 읽지 않는 연결 1개 + wrk C16 1초 | 정상 probe 진행, wrk 384개 정상. 이 runner는 무진전 종료의 정확한 시점을 입증하지 않음 |
| F06 | 기존 실제 main 통합 suite: 연결 상한1에서 수락 pause/resume | 통과, localhost 독립 서버·명시적 소켓 검사 |
| F07 | 기존 suite: worker1/queue1·읽기 장벽·추가 요청 | 503/Connection: close·EOF 및 장벽 해제 후 기존 작업·정상 종료 통과 |
| F08 | wrk C16 중 보조 부분 헤더 연결 16개 reset | wrk 384개 정상, 후속 정상 probe 성공. FD 회수의 직접 수치 검사는 이 runner에서 하지 않음 |
| F09 | 한 연결 100회 명시적 요청 후 close/EOF, wrk C1 10초 | 경계 통과, wrk 242개 정상으로 재연결 진행 확인 |
| 진단 Lua | C1 1초 | 200=25, JSON 요청/오류/백분위 출력 확인 |
| 의도적 실패 | 다른 기대 본문 파일로 validate.lua 실행 | 불일치 검출, Python 판정이 실패를 거절함 |

검증된 wrk 응답 총 5,155개에서 불일치·예상하지 않은 소켓 오류는 없었다. 별도 진단 Lua의 25개 응답과 보조 TCP 요청은 이 합계에 포함하지 않는다. F06/F07 재사용 suite는 해당 두 항목 외 기존 pipeline·GET body discard·timeout·병렬 업로드 등의 회귀도 실행했다.

최초 축약 실행에서 F09 wrk를 1초만 실행하면 100개를 넘지 못해 실패했다. 제품 장애가 아니라 검증량 부족이므로 재연결 검증의 최소 실행 시간을 10초로 올린 후 통과했다. 종료 시점의 부분 응답/소켓 오류도 정상이라고 임의 무시하지 않으며, 발생 시 원본 로그와 함께 실패 처리한다.

## 재실행 명령

환경 이미지 준비 후:

```bash
PERF_RUN_ID=functional-check tests/perf/environment.sh up
PERF_RUN_ID=functional-check tests/perf/environment.sh exec \
  python3 /source/tests/perf/functional.py \
  --base-url http://server:8000 --fixture-root /fixtures \
  --output /results/functional --duration 1 \
  --connections 1,2,4,8,16,32,64 --cases F01,F02,F03,F04,F05,F08,F09

# F06/F07은 별도 컨테이너 안에 제품 실행 파일을 빌드해 재사용 suite 실행
PERF_RUN_ID=functional-check tests/perf/environment.sh exec sh -ec '
  mkdir -p /tmp/perf-functional-build
  cp -R /source/src /source/Makefile /tmp/perf-functional-build/
  cd /tmp/perf-functional-build
  make -s
  python3 /source/tests/perf/functional.py \
    --base-url http://server:8000 --fixture-root /fixtures \
    --output /results/functional-integration --cases F06,F07 \
    --integration-binary /tmp/perf-functional-build/webserver
'
PERF_RUN_ID=functional-check tests/perf/environment.sh down

python3 tests/perf/functional.py --self-test
```

기본 기능 실행은 `--duration 10`이며, F09는 더 짧은 값을 주더라도 최소 10초다. 스레드 수와 연결 수는 기능 사례별 고정값을 사용한다. 현재 결과는 축약 핵심 검증이며 실제 부하 매트릭스·16분 유지·성능 목표 충족을 대신하지 않는다. wrk의 성공만으로 이벤트 루프 내부 전체 상태나 모든 연결의 공정성을 증명하지 않는다.
