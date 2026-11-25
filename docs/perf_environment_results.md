# wrk Linux 실행 환경 준비 결과

작성·검증일: 2026-11-25. 상태: 환경 구현·스모크 통과. 정식 기능 매트릭스와 부하 sweep은 이 작업에서 실행하지 않았다.

## 목적·범위

제품의 Linux epoll main과 wrk를 호스트 서버·저장소·포트에서 격리해 실행한다. 제품 코드·기존 Dockerfile·Makefile은 변경하지 않았다. 도구 설치는 전용 Docker 이미지 안에서만 수행한다.

- `tests/perf/Dockerfile`: Ubuntu 22.04, GCC/build-essential, Python3, wrk, procps.
- `container_entrypoint.sh`: 읽기 전용 `/source`의 src/Makefile을 컨테이너 `/tmp/perf-source`에 복사하고 기본 `-O2` 제품 빌드. fixture 생성 후 전용 `/fixtures`에서 제품 main 실행.
- `environment.sh`: build/up/exec/down/smoke 관리. 서버와 발생기를 별도 컨테이너로 실행하고 전용 Docker network로 접속한다. 호스트 포트는 공개하지 않는다.
- `environment_smoke.py`: wrk 요청 라인의 HTTP/1.1, 실제 제품 200·본문 바이트, 서버 PID namespace 접근을 확인한다.

## 호출·데이터 흐름

```text
호스트 environment.sh up
  → 전용 network·fixture volume 생성
  → server container: 소스 임시 복사·make·fixtures.py
  → /fixtures에서 webserver (PID 1, port 8000)
  → 별도 client container로 HTTP readiness 확인

호스트 environment.sh exec <명령>
  → 같은 network의 별도 client container
  → /source readonly, /fixtures readonly, /results writable
  → 서버 PID namespace 공유 → /proc/1로 서버 자원 관측

호스트 environment.sh down
  → 서버 SIGTERM·정상 종료 대기, 로그·종료 상태 보관
  → 해당 run label과 이름이 일치하는 컨테이너/network/volume만 삭제
```

서버와 발생기는 같은 Docker VM의 CPU·메모리를 공유한다. 별도 물리 장비 성능으로 해석하지 않는다. client의 PID namespace 공유는 관측을 위한 것이며 benchmark 코드는 다른 프로세스를 종료하지 않아야 한다.

## 사용 명령과 설정

```bash
./tests/perf/environment.sh build

# 각 환경은 고유 ID 사용. 같은 ID 리소스가 남아 있으면 up이 거절한다.
export PERF_RUN_ID=example
./tests/perf/environment.sh up
./tests/perf/environment.sh exec sh -c 'echo "$PERF_BASE_URL"; wrk --version'
./tests/perf/environment.sh exec python3 /source/tests/perf/functional.py \
  --base-url http://server:8000 --fixture-root /fixtures \
  --output /results/functional --duration 1 --connections 1,2 --cases F01,F02,F09
./tests/perf/environment.sh down

# 환경 자체의 짧은 자동 smoke(up/down 포함)
PERF_RUN_ID=env-smoke ./tests/perf/environment.sh smoke
```

환경 변수:

- `PERF_IMAGE`: 기본 `jungle-perf:local`.
- `PERF_RUN_ID`: 기본 local, 영문·숫자·밑줄·하이픈만 허용.
- `PERF_OUTPUT_DIR`: 기본 `build/perf-results/<run-id>`.
- `PERF_WORKERS/PERF_QUEUE/PERF_MAX/PERF_BUFFER`: up의 서버 옵션, 기본 4/64/128/32.
- exec 내부: `PERF_BASE_URL=http://server:8000`, `PERF_SERVER_PID=1`, `PERF_FIXTURE_ROOT=/fixtures`, `PERF_OUTPUT_DIR=/results`.

up이 기록하는 실행 증거: Git HEAD/status/working patch, src·Makefile·perf 도구 SHA-256(미추적 파일 포함), Docker image inspect·daemon 정보, 서버 시작 inspect, 커널·컴파일러·wrk·Python·FD 제한. down은 서버 로그와 최종 inspect를 기록한다. 미커밋 파일도 실행에 반영되므로 HEAD 하나만으로 소스가 고정됐다고 해석하지 않는다. 해시 생성 이후 소스를 수정하지 말고 실행해야 한다.

Dockerfile의 Ubuntu 태그와 apt 저장소는 시간에 따라 바뀔 수 있다. 매 실행 image ID와 패키지 버전을 기록하며 완전 동일 환경 재현에는 기록한 image artifact를 보존해야 한다. 호스트 글로벌 패키지나 커널 설정은 바꾸지 않는다.

## 실제 검증 결과

환경: Docker daemon 29.7.2, Linux 7.0.12-linuxkit aarch64, GCC 11.4.0, Python 3.10.12, wrk Debian 4.1.0-3build1. 기본 서버 옵션으로 실행했다.

| 입력·방법 | 기대 | 실제 |
|---|---|---|
| Docker image build | 전용 이미지에 빌드 도구·Python·wrk 준비 | 통과 |
| `sh -n` 두 셸 스크립트, Python py_compile | 문법 오류 없음 | 통과 |
| wrk → 테스트 TCP listener의 실제 요청 라인 캡처 | `GET / HTTP/1.1` | 통과 |
| 제품 main에 HTTP/1.1 GET small.bin | 200 및 fixture 1,024바이트 전체 일치 | 통과 |
| 별도 client의 `/proc/1/stat`, `/proc/1/fd` | 서버 PID namespace의 관측 경로 접근 가능 | 통과 |
| 실제 wrk `-t1 -c2 -d1s` → 제품 main | 요청 수행·비정상 상태/소켓 오류 없음 | 통과, 48개 응답; 성능 기준선으로 사용하지 않음 |
| down 후 서버 상태·리소스 조회 | 제품 정상 종료 exit 0, 해당 label 리소스 없음 | 통과 |

최초 이미지 빌드 중 실행 중인 셸 파일을 수정해 인터프리터 읽기 위치가 어긋나는 EOF 오류가 발생했다. 파일 수정 완료 후 문법 검사와 새 프로세스 실행으로 해소했다. 최초 요청 캡처는 wrk의 연결만 여는 주소 사전 probe를 실제 요청으로 오인해 실패했다. 빈 연결을 건너뛰고 실제 요청 바이트를 캡처하도록 테스트를 수정한 뒤 최종 smoke 전체가 통과했다. 제품 문제는 발견하지 않았다.

실행 증거는 `build/perf-results/env-smoke/`에 보관했다(빌드 결과로 Git 미포함). 소스/사용자 변경은 보존했다. 전체 회귀·sanitizer·지수 증가 부하·16분 유지 측정은 이번 환경 스모크의 통과 범위가 아니다.
