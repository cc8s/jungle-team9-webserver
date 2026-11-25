# Team 9 Web Server

크래프톤 정글 WEEK13 — C 와 POSIX 소켓 API 만으로 만드는 HTTP 웹 서버 + Proxy / Reverse Proxy.

> 🚧 구현 중

## 기능

| 영역 | 내용 | 상태 |
|---|---|---|
| TCP 소켓 서버 | `socket → bind → listen → accept` | ⬜ |
| HTTP 요청 파싱 | 요청 라인, 헤더, 본문 (GET / POST) | ⬜ |
| HTTP 응답 생성 | 상태 라인, 표준 헤더, 본문 | ⬜ |
| 정적 파일 서빙 | 루트 디렉토리, MIME 매핑, 404 / 403 / 500 | ⬜ |
| 동적 요청 처리 | | ⬜ |
| 동시 요청 처리 | | ⬜ |
| Proxy / Reverse Proxy | | ⬜ |
| 종료 처리 | SIGINT / SIGTERM, SIGPIPE 무시 | ⬜ |

## 빌드 · 실행

Linux / WSL2 / Dev Container(`.devcontainer/`, Ubuntu 22.04) 에서 빌드합니다.

```bash
make                 # ./webserver 빌드
make run             # 포트8000, ./www 루트; 워커4/큐64/연결128/수신버퍼32 MiB
./webserver --worker 4 --queue 64 --max 128 --buffer 32
# 예: make run WORKER=2 QUEUE=32 MAX=64 BUFFER=16
```

## 테스트

```bash
make test      # 기존 HTTP 회귀 + 풀/ID 단위 테스트
make test-f2   # Linux 실제 서버 통합 검증 (8000 포트 사용)
make asan      # AddressSanitizer · LeakSanitizer · UBSan 단위 테스트
make docker-test # Docker 이미지 빌드 + make test + make test-f2
```

Docker에서 실행하려면:

```bash
make docker-build
# 데이터 저장을 지속하려면 uploads_storage를 별도 마운트합니다.
docker run --rm -p 8000:8000 jungle-f2
# CLI 조정
docker run --rm -p 8000:8000 jungle-f2 ./webserver --worker 4 --queue 64 --max 128 --buffer 32
```

포트와 정적 루트는 현재 각각8000과`./www`로 고정입니다. `--buffer` 단위는 MiB이며 수신 버퍼 capacity 합계만 제한합니다. CLI 양수 범위는 `./webserver --help`로 확인할 수 있습니다. SIGINT/SIGTERM은 접수를 중단하고 큐·실행·송신 중 작업을 완료한 뒤 종료합니다. 전체 종료 기한은 없으므로 Docker의 기본 종료 유예보다 오래 걸리는 작업은 컨테이너 관리자가 강제 종료할 수 있습니다.

## 설계

main의 epoll 이벤트 루프가 여러 연결의 수신·파싱과 EPOLLOUT 전송을 맡고, pthread 워커 풀은 응답 준비·파일 읽기를 수행합니다. 연결 상한이면 accept를 일시 중지하고, 신규 요청의 큐가 포화되면503 전송 후 닫습니다. 상세 계약과 검증 범위는 [F2 설계](docs/F2_multithread/f2_design.md), [함수 흐름](docs/F2_multithread/f2_funcflow.md), [검증 결과](docs/F2_multithread/f2_testcases.md)를 참조하세요.

## 팀

| 이름 | GitHub |
|---|---|
| | [@cc8s](https://github.com/cc8s) |
| | [@bennjung](https://github.com/bennjung) |
| | [@YoonJinhoon](https://github.com/YoonJinhoon) |
