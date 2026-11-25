# wrk 부하 실행·자원 관측 도구

작성일: 2026-11-25. 제품 코드·서버 정책은 변경하지 않았다.

## 목적과 파일

- `tests/perf/run_load.py`: 지수 증가 계획 생성, wrk 실행, 단계별 원본 로그·결과 JSON/CSV·환경 기록. 기본은 dry-run이며 실제 실행은 `--execute`가 필요하다.
- `tests/perf/observe.py`: Linux `/proc/<pid>`의 CPU·RSS·FD·스레드·네트워크 namespace 카운터를 1초마다 CSV로 기록한다. CPU는 멀티코어 사용 시 100%를 넘을 수 있고 네트워크 값은 프로세스 전용 트래픽이 아니다.
- `tests/perf/percentiles.lua`: response 콜백 없이 done에서 p50/p95/p99·평균·최대 지연을 µs에서 ms로 변환하고 완료 수·오류를 출력한다.
- `tests/perf/test_runner.py`: 지수 단계, 결과 파싱, 상태별 성공률 분리, dry-run 기본 동작, Linux 관측을 검증한다.

## 실행 흐름

`plan → 환경/소스/fixture 해시 기록 → warmup → pure → 선택적 diagnostic → cooldown/recovery probe → 다음 단계`로 진행한다. 각 wrk subprocess 동안 서버와 클라이언트 PID를 따로 관측한다. 시작·중간·종료 probe는 wrk 완료 수에 포함하지 않는다. 세 번 연속 probe 실패, 서버 PID 소멸, 마지막 probe/회복 probe 실패 시 상위 단계 진행을 멈춘다. 예외·중단 시 자신이 시작한 wrk만 terminate/kill하고 관측 스레드를 정리한다. 서버 자체 생명주기는 environment.sh를 호출한 측이 소유한다.

동시성 기본 계획은 1..1024의 2배 증가 × 3회 × 작은/중간 파일 × keepalive/close의 132개 단계다. 시간 기본 계획은 30..960초 2배 증가이며 측정에서 확인한 `--safe-concurrency`를 명시해야 한다. CPU/RSS의 임의 상승을 자동으로 제품 실패로 판정하지 않는다. 표본 CSV로 자원 고갈 가능성을 검토하고 절대 성능 기준은 별도 합의한다.

HTTP 상태별 진단은 기능 담당의 `diagnose.lua`를 별도 실행한다. 순수 RPS에 진단의 200 비율을 곱하지 않는다. 진단 실행 자체의 200 수/실행시간으로 성공 RPS를 계산한다. 순수 실행의 성공 RPS는 상태를 직접 검사하지 않았으므로 비워 둔다. p99를 반복 간 합쳐 통합 백분위로 표시하지 않으며 반복 RPS의 중앙값·범위를 별도 aggregate.json에 기록한다.

## 사용 방법

```bash
# 실행하지 않고 기본 지수 매트릭스 확인
python3 tests/perf/run_load.py

# 환경은 environment.sh 담당. 같은 PERF_RUN_ID로 up/exec/down을 호출한다.
PERF_RUN_ID=my-perf tests/perf/environment.sh up
PERF_RUN_ID=my-perf tests/perf/environment.sh exec \
  python3 tests/perf/run_load.py --execute --quick-smoke --diagnostic \
  --output /results/smoke
PERF_RUN_ID=my-perf tests/perf/environment.sh down

# 전체 동시성 매트릭스는 명시적으로 실행해야 한다.
# --execute --diagnostic 을 붙이면 기본 132단계를 실제 실행한다.
```

다단계 시간 테스트는 단계별 독립 서버 재시작을 위해 `--restart-command`(shell 없이 shlex로 파싱한 argv)를 요구한다. Docker exec 내부에서 Docker socket을 제공하지 않으므로, 실제 환경에서는 호스트가 각 duration마다 새 server를 up하고 한 단계만 호출한 후 down하는 방법을 권장한다. 단일 시간 단계는 이미 새로 준비한 서버라고 간주하며 재시작 hook을 요구하지 않는다. 이 외부 재시작 여부는 실행 기록에 남겨야 한다.

환경 정보는 HEAD·dirty 상태·tracked diff·컴파일러·wrk 버전·FD 제한·cgroup 제한·fixture 해시를 기록한다. 외부 컨테이너 서버는 공유 PID namespace의 PID1로 관측한다. 서버 설정 문자열은 호출자가 제공한 기록값이므로 실제 시작 로그와 대조한다. 컨테이너에 git이 없는 경우 runner 내부 Git 명령 실패를 그대로 기록하고, 상위 environment.sh가 호스트에서 보관한 source-head.txt/source-status.txt/source-working.patch/source-files-sha256.json을 함께 사용한다. Git diff는 untracked 파일 내용까지 보관하지 않으므로 실행 스크립트는 커밋/복사해 보관해야 한다.

## 자체 검증

- macOS: self-test 6개 중 5개 통과, Linux `/proc` 자체 프로세스 검사는 플랫폼 이유로 생략.
- Linux Docker: self-test 6개 모두 통과. Python 문법 검사 통과.
- 최초 self-test에서 p95 Lua 표기를 `95.0`으로 비교한 문자열 단언이 `95`와 달라 실패했으며, 의미가 같은 실제 표기에 맞춰 검사를 수정했다.
- 실제 wrk quick smoke: warmup/pure/diagnostic 각 1초·C2. 세 실행 socket 오류·probe 실패 0, 진단 200=50/50. 서버/클라이언트 CSV·결과 JSON/CSV 생성 확인. 이 실행은 도구 연결 확인이며 성능 기준선이 아니다.

## 계획과 실행의 차이·한계

- 이번 요청의 핵심 검증은 동시성별 측정 1초·반복 1회, 시간 1/2/4/8초로 축약한다. 전체 30초×3회 매트릭스·16분 유지 테스트는 실행하지 않는다.
- quick-smoke만 cooldown 0을 허용하고, 일반 측정은 최소 10초를 요구한다.
- 워밍업은 각 단계 앞에서 실행한다. pure/diagnostic은 별도 프로세스이며 본 측정 후에는 각각 cooldown/회복 probe를 둔다.
- 서버 재시작·fixture 생성은 본 runner가 수행하지 않는다. 독립 전용 환경의 명시적 명령으로 처리한다. 축약 동시성 sweep은 같은 서버를 유지하며 각 단계 워밍업·회복 확인을 수행한다. 단계 간 캐시·할당자 상태가 이어지므로 각 실행 전 재시작하는 정식 매트릭스와 구분한다. 시간 단계는 매번 환경을 down/up해 새 서버에서 실행한다.
- 자원 관측은 Linux 전용이다. 부하 발생기와 서버가 별도 컨테이너여도 물리 CPU·메모리는 공유한다. 짧은 실행 결과로 최대 처리량·장시간 안정성을 확정하지 않는다.

C_safe 선택 시 pure·diagnostic 모두 완료 요청이 1개 이상이고, 진단 상태 합계가 완료 수와 일치하며 200이 1개 이상이고, 모든 HTTP/소켓/probe 오류가 없어야 한다. C_safe는 설정 상한 128 이하 단계 중에서 고른다. C>128의 1초 축약 실행은 15초 클라이언트 timeout보다 짧아서 미완료 대기 연결의 timeout을 관찰하지 못할 수 있다. 오류 0을 과부하 부재로 해석하지 않는다.

## 핵심 축약 동시성 실제 결과

Linux Docker의 동일 제품 서버, 1KiB·keep-alive·wrk t1에서 C=1..1024를 배증했다. 각 단계 warmup/pure/diagnostic 1초, 반복 1회, 본 측정 사이 cooldown 10초다. 각 별도 진단 실행의 결과는 다음과 같다. 순수 처리량 실행과 진단의 수치를 섞지 않는다.

| C | 진단 완료 | 200 | 503 | 진단 전체 RPS | 진단 200 RPS | socket 오류 |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 24 | 24 | 0 | 23.59 | 23.59 | 0 |
| 2 | 50 | 50 | 0 | 49.18 | 49.18 | 0 |
| 4 | 96 | 96 | 0 | 93.94 | 93.94 | 0 |
| 8 | 192 | 192 | 0 | 187.47 | 187.47 | 0 |
| 16 | 384 | 384 | 0 | 376.19 | 376.19 | 0 |
| 32 | 734 | 734 | 0 | 707.30 | 707.30 | 0 |
| 64 | 1414 | 1414 | 0 | 1355.92 | 1355.92 | 0 |
| 128 | 2937 | 2865 | 72 | 2772.70 | 2704.73 | 0 |
| 256 | 2950 | 2947 | 3 | 2774.99 | 2772.17 | 0 |
| 512 | 3071 | 3008 | 63 | 2877.47 | 2818.44 | 0 |
| 1024 | 3080 | 3076 | 4 | 2855.44 | 2851.73 | 0 |

모든 단계의 시작/중간/끝 probe와 회복 probe는 성공했다. C128부터 503을 관찰했으며 큐 포화와 연결 상한은 별개다. C>128에서 서버에 수락되지 않고 대기 중인 연결은 1초 종료와 함께 없어질 수 있으므로 socket timeout 0을 과부하 부재로 해석하지 않는다. timeout 설정은 15초이며 이번 1초 구간으로 그 경과를 검증하지 않았다.

`safe-concurrency.json`은 C<=128, pure/diagnostic 모두 완료 수 양수, 진단 상태합=완료수, 200 양수, 예상 밖 상태·소켓·probe 오류 0을 만족하는 최고 단계를 선택해 C_safe=64를 기록했다. 이는 이번 짧은 표본의 선택이며 영구 안전 용량을 의미하지 않는다.

원본 증거: `build/perf-results/runner-check/core-concurrency/`의 environment.json, 단계별 wrk.log/result.json/server.csv/client.csv, summary.csv/results.json/aggregate.json 및 safe-concurrency.json. 상위 runner-check에는 제품 빌드 소스 해시와 Git 상태가 있다. quick smoke·기능 검증·부하 핵심 표본은 서로 다른 실행이다.

## 핵심 축약 유지(Tolerance) 실제 결과

위에서 선택한 `C_safe=64`를 고정하고 1/2/4/8초를 단계별로 `environment.sh down/up`해 새 서버에서 실행했다(1KiB·keep-alive·wrk `-t1`, 각 단계 warmup 1초 → pure → diagnostic, cooldown 10초). 순수 실행과 진단 실행의 수치를 분리해 기록한다.

| 시간 | 단계 | 완료 요청 | 200 | 전체 RPS | 200 RPS | p50 (ms) | p95 (ms) | p99 (ms) | socket/probe 오류 | 서버 RSS (KiB) | 서버 FD |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1초 | pure | 1472 | — | 1425.61 | — | 43.11 | 52.50 | 53.45 | 0 / 0 | 2716 | 7–70 |
| 1초 | diagnostic | 1472 | 1472 | 1417.03 | 1417.03 | 43.83 | 49.05 | 49.92 | 0 / 0 | 2716–2844 | 6–105 |
| 2초 | pure | 2944 | — | 1449.02 | — | 43.12 | 49.95 | 61.68 | 0 / 0 | 2772 | 7–70 |
| 2초 | diagnostic | 2898 | 2898 | 1400.69 | 1400.69 | 44.02 | 51.33 | 59.82 | 0 / 0 | 2772 | 6–74 |
| 4초 | pure | 5653 | — | 1389.42 | — | 43.02 | 59.33 | 105.55 | 0 / 0 | 2780–2908 | 6–70 |
| 4초 | diagnostic | 5890 | 5890 | 1442.33 | 1442.33 | 43.24 | 49.20 | 54.55 | 0 / 0 | 2908–3036 | 7–70 |
| 8초 | pure | 11586 | — | 1443.79 | — | 43.74 | 49.47 | 53.09 | 0 / 0 | 2844 | 6–79 |
| 8초 | diagnostic | 11302 | 11302 | 1400.63 | 1400.63 | 43.59 | 53.67 | 84.63 | 0 / 0 | 2844 | 6–95 |

전 구간에서 HTTP/소켓 오류 및 probe 실패는 0이었고, 8초 단일 실행 동안 서버 RSS는 2844 KiB로 일정하게 유지됐다. 이는 짧은 축약 표본이며 30~960초 장시간 안정성 입증을 대신하지 않는다.

원본 증거: `build/perf-results/runner-duration-{1,2,4,8}/core-duration/`.
