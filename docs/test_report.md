# 테스트 수행 내역 및 결과 보고서

작성일: 2026-11-25  
기준 브랜치/커밋: `jung/main` (`056e36e`, 작업 트리 미커밋 변경 포함)  
참고: 아래 결과는 실제 실행 완료된 검증만 **통과**로 기록하며, 축약 표본·부분 검증·미실행 항목을 명확히 구분한다.

---

## 1. 총괄 요약

| 영역 | 주요 검증 범위 | 실행 환경 | 상태 | 핵심 결과 |
|---|---|---|---|---|
| **F1: HTTP 요청/응답·POST·업로드** | 파서, Host B안, 크기/기한, 정적 GET, 경로 보호, JSON/폼, Base64 업로드, 오류 주입, Persistent Connection | macOS + Linux Docker | **통과 (일부 부분/미실행)** | C 단위/통합 테스트 및 Python 업로드 경계 테스트 전체 통과, ASan/LSan/UBSan 통과. Persistent 일부 한계 항목 미실행 |
| **F2: epoll + pthread 동시성** | 제한 작업 큐, 워커 풀, EPOLLOUT 부분 송신, 연결 상한 pause/resume, 503 과부하, 빈 EOF 정리, SIGTERM 정상 종료 | macOS + Linux Docker (arm64) | **통과 (일부 한계 미실행)** | `make test`, `make test-f2`, `make asan`, TSan(`seccomp=unconfined`), UID1000 권한 검사 통과 |
| **wrk 격리 실행 환경** | 전용 Ubuntu 22.04 Docker 이미지, 서버/발생기 분리, `/proc/1` 자원 관측, 정상 종료·정리 | Linux Docker (arm64) | **통과 (환경 스모크)** | HTTP/1.1 200 본문 1 KiB 일치, 48응답 오류 0, 종료 코드 0 및 라벨 리소스 정리 확인 |
| **wrk 기능 검증 (F01~F09)** | 다중 동시 GET, 스레드별 식별, 느린 헤더/idle/무수신/강제 종료 공존, 100요청 재연결, F06/F07 통합 보완 | Linux Docker (arm64) | **통과 (축약 1초, F09 10초)** | wrk 검증 응답 **5,155건** 본문/길이/상태 불일치 0, 예상 밖 소켓 오류 0 |
| **wrk 부하 A: 동시성 지수 증가** | 1 KiB, keep-alive, `C=1..1024` (11단계), 각 1초×1회 (`pure`/`diagnostic` 분리) | Linux Docker (arm64) | **완료 (핵심 축약 표본)** | `C=1..64` 무오류 선형 증가(최대 ~1,487 RPS), `C=128`부터 503 관측 → **`C_safe=64`** 선택 |
| **wrk 부하 B: 유지(Tolerance)** | `C_safe=64` 고정, `D=1/2/4/8초` (단계별 서버 재시작, `pure`/`diagnostic` 분리) | Linux Docker (arm64) | **완료 (핵심 축약 표본)** | 전 구간 200 RPS **1,401~1,442** 유지, 오류/probe 실패 0, 서버 RSS 2.7~3.0 MiB 안정 |
| **정식 전체 부하 매트릭스·장시간 유지** | 64 KiB, `Connection: close`, 30초×3회(132단계), 30~960초(최장 16분) 유지, SLO 확정 | — | **미실행** | 축약 실행 결과로 영구 용량·장시간 안정성·성능 목표 충족을 확정하지 않음 |

---

## 2. 테스트 환경 및 소스 기준

### 실행 환경
- **호스트**: macOS (Darwin, 일반 사용자 실행 포함)
- **Linux Docker 환경**: Docker daemon `29.7.2`, 커널 `Linux 7.0.12-linuxkit aarch64`
- **도구 체인**: GCC `11.4.0` (`-std=c11 -Wall -Wextra -g -O2 -pthread`), Python `3.10.12`, wrk `debian/4.1.0-3build1 [epoll]`
- **컨테이너 자원·제한**: `ulimit -n` = `1048576`, 서버와 wrk 부하 발생기는 별도 컨테이너로 분리하되 동일 Docker VM의 CPU·메모리를 공유
- **제품 서버 기본 설정**: `./webserver --worker 4 --queue 64 --max 128 --buffer 32` (포트 8000, 정적 루트 `www`, 업로드 `uploads_storage`)

### 소스 상태
- 기준 커밋: `056e36e` (`test: wrk 기능 검증 도구와 격리 시나리오 추가`)
- wrk 부하 실행기(`tests/perf/run_load.py`, `observe.py`, `percentiles.lua`, `test_runner.py`, `README.md`) 및 문서/일부 작업 트리 변경(`src/http_connection.c` 등)이 미커밋 상태로 존재하며, 각 perf 실행 디렉토리에 `source-head.txt`, `source-status.txt`, `source-working.patch`, `source-files-sha256.json`으로 실행 당시 스냅샷을 보존했다.

---

## 3. F1: HTTP 처리 · POST · 업로드 · Persistent Connection 검증

상세 이력: [f1_testcases.md](F1_processhttp/f1_testcases.md)

### 3.1 자동화 테스트 구성 및 통과 범위
`make test`로 아래 C 테스트 바이너리와 Python 도구를 일괄 실행했으며, macOS(`ASan/UBSan`) 및 Linux Docker(`ASan/LSan/UBSan`)에서 통과했다.

| 테스트 파일 | 주요 검증 항목 | 결과 |
|---|---|---|
| `tests/test_http_request.c` | 1바이트 증분 파싱, NUL/CRLF 본문, 5,000B 버퍼 확장, EOF 구분, Host B안 정책(IPv6 zone ID 차단 포함), Content-Length/Transfer-Encoding 오류, 8 KiB 라인·32 KiB 헤더 경계 및 문법 오류 우선순위, 헤더 5초·POST 본문 10초 절대 기한, GET 본문 정책 | 통과 |
| `tests/test_http_response.c` | API 경계값(`SIZE_MAX`, 414/500), 9종 MIME 및 하위 경로 `/css/main.css?v=1`, 경로 보호(404/403, `..`/`%00`/심볼릭 링크/`chmod 0000`/루트 누락 500), 예약 API 3종 × 9메서드 405(`Allow: POST`), 업로드 파일 GET 차단, 16 KiB 청크 전송·5초 쓰기 타임아웃 | 통과 |
| `tests/test_path_bounds.c` | `decode_path` 소형 버퍼 경계, `/`→`/index.html` 치환, 앞뒤 sentinel 보존 | 통과 |
| `tests/test_json.c` | JSON 파싱·재직렬화(큰 정수·한글·NUL·surrogate), 중복 키 400, 깊이(16/17)·객체 필드(100/101)·배열(1,000/1,001)·전체 값(10,000/10,001) 제한, 폼 URL 디코딩·NUL(`%00`) 거부·빈 세그먼트 무시·빈 키 거부·100/101 필드 제한 | 통과 |
| `tests/test_post.c` | `/api/users`·`/api/form`·`/api/upload` 정상/오류 처리, Content-Type(415/UTF-8), 0바이트 본문 400(`invalid_body`), 업로드 `.bin`/`.json` 쌍·`0400` 권한·경로 탈출 방지·중복 파일명 독립 ID·Base64 검증·비문자열/빈 `content_type` 거절 | 통과 |
| `tests/test_post_boundaries.c` | 본문 정확히 1 MiB 성공 및 초과 413, 제어문자/특수문자 JSON 이스케이프 재파싱, 응답 Content-Type·길이 검증 | 통과 |
| `tests/tools/test_upload_fixture.py` | 업로드 도구 JSON `1 MiB - 1` / `1 MiB` 생성 및 `+1` 사전 거절 검증 | 통과 |
| `tests/test_transfer_timing.c` | 제어된 I/O·단조 시계 기반 11개 시나리오(기한 경과 직후 수신 4분기, 양수 send 갱신/EAGAIN 무갱신 3분기, 저장 후 응답 송신 실패 시 파일 보존 4분기) | 통과 |
| `tests/test_fault_recovery.c` | 26개 오류 주입 시나리오(`poll`/`recv`/`send`/`read` EINTR·실패, 시각 역행/충돌 ID 재시도, 파일 생성·쓰기·`close`·`fchmod` 실패 시 정리 및 기존 파일 보존) | 통과 |
| `tests/test_persistent.c` | 같은 연결 GET/POST 혼합 재사용, 파이프라이닝 경계, 연결당 100요청 상한(`Connection: close`), GET 본문 즉시 응답 후 폐기·10초 기한, 오류 응답 시 `close`, 유휴 5초 종료 및 다음 헤더 5초 408 | 통과 |
| `tests/test_stability.c` | 4,400회 반복 FD/RSS 계측(일반 실행 워밍업 후 RSS 증가 0, FD 5 고정; Linux LSan 통과), 실제 loopback TCP 2 MiB 느린 수신 일치(macOS ~10.4s / Linux ~13.5s) 및 8 MiB 무수신 실패(~5.0~5.4s), SIGKILL 잔존 관찰 | 부분 검증 / 관찰 완료 |
| `tests/test_smoke.c` | 빌드·테스트 파이프라인 스모크 | 통과 |

### 3.2 F1 한계 및 미실행 항목
- **부분 검증**: `test_stability.c` 4,400회 반복 및 LSan은 유한 반복 검증이며 장시간 운용 누수 부재를 입증하지 않음.
- **관찰 완료 (내구성 미보장)**: `SIGKILL` 강제 종료 시 부분 `.bin`(2B)·빈 `.json`(0B)·`0600` 잔존 확인(파일 쌍 원자성·`fsync` 내구성 미보장).
- **미실행**: Persistent GET 본문 폐기 단계 I/O 강제 실패, 장시간 재사용 FD 계측, 새 연결 루프의 단독 Linux 실행.

---

## 4. F2: Linux epoll + pthread 워커 풀 동시성 검증

상세 이력: [f2_testcases.md](F2_multithread/f2_testcases.md)

### 4.1 단계별 및 최종 검증 결과

| 테스트 대상 / 명령 | 검증 내용 | 환경 | 결과 |
|---|---|---|---|
| `tests/test_upload_id.c` | 스레드별 업로드 ID 독립성 및 `UINT64_MAX` 경계·소진 상태 | macOS / Linux Docker | 통과 |
| `tests/test_thread_pool.c` | 워커 동시 실행 중 제한 큐 포화 즉시 반환 및 장벽 해제 후 4작업 완료 유실 없음 | macOS / Linux Docker | 통과 |
| `tests/test_accept_recovery.c` | `accept4`의 `ECONNABORTED`/`EPROTO`/pending network errors 재시도, `EMFILE` 명시적 종료 | Linux Docker (일반 + ASan/LSan/UBSan) | 통과 |
| `tests/test_pool_failure.c` | allocation 실패 3종·2번째 `pthread_create` 실패 시 부분 워커 join 및 자원 정리 | Linux Docker (일반 + ASan/LSan/UBSan) | 통과 |
| `tests/test_receive_close.c` | 빈 EOF 및 `ECONNRESET`/`EIO`는 워커 제출 0회·즉시 정리, 부분 요청 EOF 1회, 완성 요청+`SHUT_WR` 정상 1회 | Linux Docker (일반 + ASan/LSan/UBSan) | 통과 |
| `make test-f2` (`tests/tools/test_epoll_server.py`) | 실제 `webserver` main으로 느린 헤더, 워커 수 초과 idle, 버퍼 내 파이프라인, 늦은 GET 본문 뒤 POST, `SHUT_WR`, 24 병렬 업로드, 연결 상한 1 pause/resume, 수신 버퍼 1 MiB 초과 close, 잘못된 CLI 거부, `LD_PRELOAD` 읽기 장벽 기반 큐 포화(503/close) + SIGTERM 후 256 KiB 청크·큐 작업 drain 정상 종료, 100요청 파이프라인, 5s/10s/idle 만료, 큐 포화 중 빈 EOF 무응답 종료 | Linux Docker (일반, ASan/LSan/UBSan, TSan) | 통과 |
| 권한 거부 (`chmod 0000` / 업로드 쓰기 거부) | root 컨테이너 생략 보완을 위해 UID 1000 컨테이너에서 `test_http_response`·`test_post` 실행 | Linux Docker (UID 1000) | 통과 |
| 무수신 대형 전송 공존 | 워커 1, 16 MiB 파일, 수신버퍼 1024 무수신 연결 공존 중 다른 GET 2초 이내 완료 및 SIGTERM 3초 이내 종료 | Linux Docker | 통과 |
| 기본 Docker 이미지 업로드 | 사전 준비된 `/app/uploads_storage`에 업로드 201 및 정상 종료 | `jungle-f2` 이미지 | 통과 |
| ThreadSanitizer (TSan) | Docker 기본 seccomp 제약으로 `--security-opt seccomp=unconfined` 컨테이너에서 `test_thread_pool`·`test_upload_id` 및 실제 main 통합 전체 실행 | Linux Docker (TSan) | 통과 |

### 4.2 F2 미실행·제한 사항
- 연결 `generation` `2^32`회 소진 실험 및 실제 호스트 전체 FD 고갈 실험 미실행.
- 정상 종료(`SIGTERM`)는 진행 중/큐 대기 작업 완료까지 대기하며 전체 종료 타임아웃 상한을 두지 않음.

---

## 5. wrk 격리 환경 및 기능 검증 결과 (F01~F09)

상세 이력: [perf_environment_results.md](perf_environment_results.md), [perf_functional_results.md](perf_functional_results.md)

### 5.1 격리 환경 스모크 (`env-smoke`)
- `tests/perf/Dockerfile` 이미지 빌드, 셸 문법(`sh -n`)·Python 문법(`py_compile`) 통과.
- wrk 실제 요청 라인 `GET / HTTP/1.1` 캡처 통과.
- 제품 main 대상 `/perf/small.bin`(1 KiB) 200 응답 및 전체 바이트 일치 통과.
- 공유 PID namespace에서 별도 client 컨테이너의 `/proc/1/stat`, `/proc/1/fd` 접근 확인.
- 짧은 wrk 스모크(`-t1 -c2 -d1s`): 48개 응답, 상태/소켓 오류 0.
- `environment.sh down` 후 서버 `exit 0` 정상 종료 및 라벨 리소스 잔존 없음 확인.

### 5.2 wrk 기능 검증 (`functional-smoke`)
- 실행 조건: F01~F05·F08은 1초 축약, F09는 100요청 경계 초과 재연결 확인을 위해 10초 실행.
- **총 wrk 검증 응답: 5,155건** (본문·길이·상태 불일치 **0**, 예상 밖 소켓 오류 **0**).

| ID | 시나리오 및 입력 | 검증 응답 수 | 결과 및 비고 |
|---|---|---:|---|
| Python self-test | fixture 생성/해시, JSON 불일치 거절 판정 | — | 통과 |
| **F01** | 정상 GET `small.bin`, `C=1/2/4/8/16/32/64` 각 1초 | 25 / 50 / 96 / 192 / 384 / 769 / 1,472 (합 2,988) | 통과 (불일치 0) |
| **F02** | wrk 4스레드·16연결, 스레드별 서로 다른 1 KiB fixture 식별 | 384 | 통과 (물리 연결이 아닌 스레드별 식별) |
| **F03** | 부분 헤더 8개 + wrk `C=16` 1초 | 384 | 통과 (정상 probe `0.80 ms` 만료 전 완료, 부분 헤더 각 408) |
| **F04** | idle keep-alive 8개 + wrk `C=16` 1초 | 389 | 통과 (정상 probe 진행, idle EOF 확인) |
| **F05** | 16 MiB 무수신 연결 1개 + wrk `C=16` 1초 | 384 | 통과 (정상 probe `0.68 ms`; 정확한 송신 timeout 시점은 미입증) |
| **F06** | 기존 실제-main 통합 수트: 연결 상한 1 pause/resume | — | 통과 (wrk 아닌 명시적 소켓 통합 검사) |
| **F07** | 기존 실제-main 통합 수트: worker 1 / queue 1 읽기 장벽 포화 | — | 통과 (503/close 및 장벽 해제 후 drain 정상 종료) |
| **F08** | wrk `C=16` 중 부분 헤더 16개 강제 reset + 회복 probe | 384 | 통과 (회복 probe `1.47 ms`; 사례별 FD 수치 직접 검사는 미수행) |
| **F09** | 단일 연결 100회 명시적 요청 close/EOF + wrk `C=1` 10초 | 242 (+명시적 100) | 통과 (100요청 경계 및 wrk 재연결 확인) |
| 진단 Lua | `diagnose.lua` `C=1` 1초 | 25 (합계 미포함) | 통과 (`200=25`, JSON 백분위 출력) |
| 의도적 실패 | 다른 기대 본문 파일로 `validate.lua` 실행 | — | 통과 (불일치 검출 및 종료 코드 1 거절 확인) |

---

## 6. wrk 부하 실행기 및 핵심 축약 부하 테스트 결과

상세 이력: [perf_runner_results.md](perf_runner_results.md), [perf_test.md](perf_test.md)

### 6.1 부하 실행기 자체 검증
- `tests/perf/test_runner.py`: macOS 5/6 통과(`/proc` 검사 플랫폼 생략), Linux Docker **6/6 전체 통과**.
- Quick smoke (`C=2`, 1초): warmup/pure/diagnostic 모두 소켓·probe 오류 0, 진단 200=50/50, CSV/JSON 산출물 정상 생성.

### 6.2 부하 A — 동시성 지수 증가 (`C=1..1024`, 1 KiB, keep-alive, `-t1`, 1초×1회)

동일 제품 서버(`workers=4, queue=64, max=128, buffer=32 MiB`)를 유지한 채 각 단계 `warmup(1s) → pure(1s) → diagnostic(1s)`과 본 측정 사이 `cooldown 10s` 및 시작/중간/끝/회복 probe를 수행했다. **순수 실행(`pure`)과 상태별 진단 실행(`diagnostic`)의 수치를 분리해 보고한다.**

#### 순수 처리량 실행 (`pure`, `percentiles.lua`, `response` 콜백 없음)

| C | 구간 | 완료 요청 | 전체 RPS | p50 (ms) | p95 (ms) | p99 (ms) | status 오류 | socket 오류 | 서버 RSS (KiB) | 서버 CPU 최대 | 서버 FD | 클라 CPU 최대 |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 상한 이내 | 24 | 23.58 | 42.74 | 45.87 | 45.91 | 0 | 0 | 1,880 | 2.0% | 6–7 | 1.0% |
| 2 | 상한 이내 | 48 | 46.66 | 42.19 | 46.69 | 52.34 | 0 | 0 | 1,880 | 1.0% | 6–8 | 1.0% |
| 4 | 상한 이내 | 96 | 94.44 | 41.80 | 46.20 | 47.23 | 0 | 0 | 1,880 | 2.0% | 6–10 | 0.0% |
| 8 | 상한 이내 | 200 | 190.35 | 41.96 | 46.43 | 52.10 | 0 | 0 | 1,880 | 2.0% | 8–14 | 1.0% |
| 16 | 상한 이내 | 384 | 370.54 | 42.70 | 50.83 | 59.67 | 0 | 0 | 1,880 | 4.0% | 7–22 | 1.0% |
| 32 | 상한 이내 | 758 | 742.31 | 43.29 | 49.38 | 54.17 | 0 | 0 | 2,136 | 5.0% | 6–38 | 3.0% |
| **64** | **상한 이내 (`C_safe`)** | **1,536** | **1,486.64** | **42.94** | **50.64** | **53.99** | **0** | **0** | **2,648** | **8.0%** | **7–70** | **5.0%** |
| 128 | 상한 이내 | 3,201 | 3,102.49 | 43.13 | 46.37 | 47.76 | 116 | 0 | 3,544–3,800 | 13.0% | 6–134 | 7.0% |
| 256 | 과부하 (`>max`) | 3,084 | 2,945.86 | 41.96 | 45.22 | 47.63 | 0 | 0 | 3,616–4,000 | 14.9% | 6–134 | 6.0% |
| 512 | 과부하 (`>max`) | 3,092 | 2,914.09 | 42.33 | 46.20 | 421.32 | 10 | 0 | 3,532–4,044 | 17.0% | 6–134 | 7.0% |
| 1024 | 과부하 (`>max`) | 3,206 | 2,990.90 | 43.08 | 48.37 | 747.88 | 74 | 0 | 3,828–4,212 | 13.9% | 6–134 | 8.9% |

#### 상태별 진단 실행 (`diagnostic`, `diagnose.lua`)

| C | 구간 | 진단 완료 | 200 | 503 | 전체 RPS | **200 성공 RPS** | p50 (ms) | p95 (ms) | p99 (ms) | socket/probe 오류 | 서버 RSS (KiB) | 서버 FD |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 상한 이내 | 24 | 24 | 0 | 23.59 | **23.59** | 41.69 | 47.22 | 51.82 | 0 / 0 | 1,880 | 7 |
| 2 | 상한 이내 | 50 | 50 | 0 | 49.18 | **49.18** | 41.62 | 45.17 | 47.72 | 0 / 0 | 1,880 | 7–8 |
| 4 | 상한 이내 | 96 | 96 | 0 | 93.94 | **93.94** | 41.96 | 47.24 | 47.82 | 0 / 0 | 1,880 | 6–10 |
| 8 | 상한 이내 | 192 | 192 | 0 | 187.47 | **187.47** | 42.72 | 48.47 | 49.55 | 0 / 0 | 1,880 | 6–14 |
| 16 | 상한 이내 | 384 | 384 | 0 | 376.19 | **376.19** | 41.86 | 45.87 | 48.49 | 0 / 0 | 1,880 | 6–22 |
| 32 | 상한 이내 | 734 | 734 | 0 | 707.30 | **707.30** | 42.31 | 63.37 | 78.76 | 0 / 0 | 2,136 | 6–38 |
| **64** | **상한 이내 (`C_safe`)** | **1,414** | **1,414** | **0** | **1,355.92** | **1,355.92** | **43.04** | **64.27** | **67.25** | **0 / 0** | **2,648** | **6–70** |
| 128 | 상한 이내 | 2,937 | 2,865 | 72 | 2,772.70 | **2,704.73** | 41.90 | 84.66 | 123.33 | 0 / 0 | 3,532–3,916 | 6–134 |
| 256 | 과부하 (`>max`) | 2,950 | 2,947 | 3 | 2,774.99 | **2,772.17** | 43.36 | 52.83 | 54.75 | 0 / 0 | 3,556–3,812 | 6–134 |
| 512 | 과부하 (`>max`) | 3,071 | 3,008 | 63 | 2,877.47 | **2,818.44** | 43.09 | 245.15 | 672.06 | 0 / 0 | 3,656–4,040 | 6–134 |
| 1024 | 과부하 (`>max`) | 3,080 | 3,076 | 4 | 2,855.44 | **2,851.73** | 42.32 | 45.70 | 47.23 | 0 / 0 | 3,828–4,084 | 6–134 |

- **`C_safe` 선택**: 상한 128 이하 구간 중 `pure`/`diagnostic` 모두 완료 요청 양수, 진단 상태 합=완료 수, 모든 HTTP/소켓/probe 오류 0을 만족한 최고 단계로 **`C_safe = 64`**를 자동 선택(`safe-concurrency.json`).
- **핵심 관찰**:
  1. `C=1..64` 구간에서는 동시 연결 2배 증가에 따라 처리량이 선형적으로 증가(`~24 → ~1,487 RPS`)했고 503·소켓·probe 오류가 0이었다.
  2. `C=128`에서 처리량이 `~2,705~3,102 RPS` 수준에 도달함과 동시에 작업 큐(64) 포화로 **503 응답**이 최초 관측(`diagnostic` 72건, `pure` status 오류 116건)됐다.
  3. `C=256..1024`에서는 서버 `max=128` 제한이 작동해 서버 FD 상한이 **134**(기본 FD 6 + 활성 연결 128)에서 고정됐고, RPS는 `~2,772~2,991`로 정체되며 `C=512/1024`에서 꼬리 지연(`p99`)이 **421~748 ms**까지 급등했다.
  4. 전 구간에서 `p50`이 **~42~44 ms**로 일정하게 유지되어 단일 연결당 ~24 req/s 상한이 형성됐다(클라이언트·서버 CPU는 최대 19~25% 미만으로 CPU 포화가 아닌 네트워크/소켓 왕복 지연 패턴으로 보이며, 원인 분석은 별도 조사 필요).
  5. `C>128`에서 `socket_errors=0`인 이유는 1초 축약 구간이 클라이언트 timeout(15초)보다 짧아 수락 대기 연결이 timeout 전 종료됐기 때문이며, 과부하 부재로 해석하지 않는다.
  6. 모든 단계의 시작/중간/끝 probe 및 회복 probe는 성공했다.

### 6.3 부하 B — 유지(Tolerance) 지수 증가 (`C_safe=64`, `D=1/2/4/8초`, 단계별 서버 재시작)

각 시간 단계마다 `environment.sh down/up`으로 새 서버 컨테이너를 기동해 독립 실행했다(`runner-duration-1`, `2`, `4`, `8`).

| 시간 | 단계 | 완료 요청 | 200 | 전체 RPS | 200 성공 RPS | p50 (ms) | p95 (ms) | p99 (ms) | 오류 (status/sock/probe) | 서버 RSS (KiB) | 서버 CPU 최대 | 서버 FD | 클라 CPU 최대 |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|---:|
| **1초** | pure | 1,472 | — | 1,425.61 | — | 43.11 | 52.50 | 53.45 | 0 / 0 / 0 | 2,716 | 7.0% | 7–70 | 3.0% |
| **1초** | diagnostic | 1,472 | 1,472 | 1,417.03 | 1,417.03 | 43.83 | 49.05 | 49.92 | 0 / 0 / 0 | 2,716–2,844 | 8.0% | 6–105 | 5.0% |
| **2초** | pure | 2,944 | — | 1,449.02 | — | 43.12 | 49.95 | 61.68 | 0 / 0 / 0 | 2,772 | 9.9% | 7–70 | 6.0% |
| **2초** | diagnostic | 2,898 | 2,898 | 1,400.69 | 1,400.69 | 44.02 | 51.33 | 59.82 | 0 / 0 / 0 | 2,772 | 10.0% | 6–74 | 9.0% |
| **4초** | pure | 5,653 | — | 1,389.42 | — | 43.02 | 59.33 | 105.55 | 0 / 0 / 0 | 2,780–2,908 | 22.0% | 6–70 | 12.0% |
| **4초** | diagnostic | 5,890 | 5,890 | 1,442.33 | 1,442.33 | 43.24 | 49.20 | 54.55 | 0 / 0 / 0 | 2,908–3,036 | 10.0% | 7–70 | 6.0% |
| **8초** | pure | 11,586 | — | 1,443.79 | — | 43.74 | 49.47 | 53.09 | 0 / 0 / 0 | 2,844 | 10.0% | 6–79 | 7.0% |
| **8초** | diagnostic | 11,302 | 11,302 | 1,400.63 | 1,400.63 | 43.59 | 53.67 | 84.63 | 0 / 0 / 0 | 2,844 | 24.9% | 6–95 | 20.9% |

- **유지 구간 요약**: 1초→8초(최대 11,586 요청) 전 단계에서 서버 생존, probe 지연 `1.0~14.8 ms` 전량 성공, HTTP/소켓 오류 0을 기록했다. 8초 단일 실행 내 서버 RSS는 **2,844 KiB로 고정**되어 단기 메모리 누수 징후가 없었고, 처리량도 `~1,400~1,449 RPS` 범위에서 안정적이었다.

---

## 7. 미실행 항목 및 향후 과제

1. **정식 wrk 부하 매트릭스 (미실행)**
   - 전체 132단계 매트릭스: `1 KiB` / `64 KiB` × `keep-alive` / `Connection: close` × `C=1..1024` × **30초 × 3회 반복** (단계별 서버 재시작 포함).
   - 장시간 유지(tolerance) 테스트: `D = 30초 → 60초 → 120초 → 240초 → 480초 → 960초(16분)`.
   - 절대 성능 목표(성공 RPS, p95/p99 상한, 허용 오류율, RSS 증가율) 합의.
2. **성능 병목 원인 확인 (제안)**
   - 전 구간 `p50 ≈ 42~44 ms` 발생 원인(예: `TCP_NODELAY` 미설정으로 인한 Nagle + Linux 40 ms Delayed ACK 상호작용 여부) 점검.
3. **기존 기능 검증 잔여 한계**
   - F1 Persistent: GET 본문 폐기 I/O 강제 실패, 장시간 재사용 FD 계측, 새 연결 루프 단독 Linux 실행.
   - F2 동시성: `generation` `2^32` 소진, 실제 호스트 FD 고갈 실험.
   - wrk 기능 보완: F05 정확한 송신 timeout 시점, F08 사례별 FD 회수 수치 직접 검증.
4. **로드맵 잔여 필수 기능**
   - [phase.md](phase.md) 기준 Proxy / Reverse Proxy 구현 및 관련 테스트(백엔드 무응답/타임아웃, 라우팅, 로드 밸런싱 등).

---

## 8. 원본 증거 및 재현 명령

### 원본 증거 디렉토리 (`build/perf-results/`, Git 미추적)
- 환경 스모크: `build/perf-results/env-smoke/`
- 기능 검증: `build/perf-results/functional-smoke/` (`functional/`, `functional-f01-rest/`, `functional-integration/`)
- 동시성 지수 증가: `build/perf-results/runner-check/core-concurrency/` (`summary.csv`, `results.json`, `aggregate.json`, `safe-concurrency.json`, 단계별 `wrk.log`/`server.csv`/`client.csv`)
- 유지(Tolerance) 1/2/4/8초: `build/perf-results/runner-duration-{1,2,4,8}/core-duration/`

### 주요 재현 명령
```bash
# 1) F1/F2 단위·통합 회귀 테스트
make test
make asan                  # Linux 환경 ASan/LSan/UBSan
make docker-test           # Linux Docker에서 make test + make test-f2

# 2) wrk 격리 환경 스모크
PERF_RUN_ID=env-smoke ./tests/perf/environment.sh smoke

# 3) wrk 부하 실행기 self-test 및 dry-run 계획 확인
python3 tests/perf/test_runner.py
python3 tests/perf/run_load.py
```
