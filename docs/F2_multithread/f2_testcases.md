# F2. epoll + pthread 검증 계획

## 이번 계획 작업의 실제 확인

- main은 handle_connection을 직접 호출하고, 해당 함수는 수신/응답/다음 요청을 반복한다.
- http_receive_request 내부의 poll 대기는 이벤트 루프에서 재사용할 수 없다. http_parse_request는 대기 없는 파서다.
- 파서는 POST만 전체 본문을 기다린다. GET의 COMPLETE는 헤더 완료이므로 전체 본문 완료와 구분해야 한다.
- 기존 http_send_all은 poll(POLLOUT)을 사용한다. 확정 설계에서는 main이 이 루프를 호출하지 않고 EPOLLOUT 재개형 송신으로 분리할 계획이다.
- store_upload 공유 sequence는 멀티 스레드 적용 전 변경 필요. Makefile -pthread는 이미 존재.
- uname -s 결과 Darwin. Linux epoll 실행 테스트는 이번 작업에서 하지 않았다.
- 제품 코드·실행 결과는 변경하지 않았다. git diff --check로 문서 공백 오류를 검사한다.

## 단계별 실행 예정

| 단계/상황 | 기대 결과 | 방법 |
|---|---|---|
| 수신 step: 헤더·POST 분할 | 부족 시 즉시 반환, 완성 시 한 번 COMPLETE | socketpair + 논블로킹 호출, 기존 파서 테스트 |
| A는 느린 헤더, B는 완성 요청 | B가 A의 헤더/idle 기한을 기다리지 않고 처리 | Linux 실제 서버에 독립 TCP 연결 |
| N보다 많은 idle 연결 | idle 연결이 워커를 점유하지 않음 | N+개 keep-alive와 새 GET 동시 실행 |
| 상한 MAX_CONNECTIONS | queued/working/idle 포함 상한 준수, 공간 생기면 수락 재개 | 기본 128개와 작은 테스트 상한으로 pause/resume 계측 |
| 작업 큐 포화 | 완료 요청에 503/Connection: close 전송 후 종료, 기존 작업 정상 처리 | 워커4개 장벽 + 대기 큐64개 포화 후 추가 요청 |
| 알림 병합·다수 워커 완료 | 완료 건 유실 없이 큐를 모두 비움 | eventfd 한 번 알림에 여러 완료 삽입 |
| 같은 연결에 여러 요청 | 동시 작업 한 개, 응답 순서 보존 | 파이프라이닝 GET/POST, 작업 시작/완료 기록 |
| 후속 요청이 사용자 버퍼에만 존재 | 새 EPOLLIN 없이 다음 요청 처리 | 한 send에 여러 요청, 완료 후 즉시 파싱 확인 |
| GET 본문이 늦게 도착 | 응답은 먼저, 본문만 폐기, 후속 요청 보존 | 분할 GET body + 다음 요청 전송 |
| 데이터+RDHUP, 작업 중 연결 종료 | 완성 요청의 응답 정책 준수, 이중 close/UAF 없음 | shutdown(SHUT_WR), reset, 워커 장벽 |
| FD 번호 재사용·오래된 이벤트 | 이전 연결 이벤트가 새 연결에 적용되지 않음 | 강제 재사용과 generation 검사 |
| 모든 수신/idle 기한 | 바이트마다 연장 안 됨, 이벤트 없어도 만료 처리 | 시계 제어 + 실제 무데이터 연결 |
| 오류·timeout 작업 | 완성 요청이 아니어도 기존 400/408 등 응답, idle EOF는 무응답 | malformed/partial 요청 회귀 |
| 병렬 업로드 | ID 데이터 레이스 없음, 파일쌍·바이트 일치 | 여러 워커 POST와 기존 ID 충돌 주입 |
| 버퍼 예산 초과 | 수신 버퍼32 MiB 예산 초과 연결 close, 다른 연결 진행 | 다수 연결에서 큰 부분 본문 전송 |
| 초기화 일부 실패·종료 | 워커/큐/FD/객체 회수, 완료 큐 교착 없음 | pthread_create/epoll_ctl 실패 주입, SIGTERM |
| 기존 HTTP 회귀 | F1 응답·기한·파일 정책 유지 | make test + 실제 Linux main 경유 통합 테스트 |

동시성 구현 후 Linux에서 ASan/UBSan과 별도 TSan 빌드로 검사한다. sanitizer 실행 가능 여부와 실제 결과를 기록한다. 처리량·지연·오류 수는 같은 요청·동시성·환경에서 측정한다. 이 표는 실행 전 계획이며 통과 결과가 아니다.

## 정책 확정 후 추가 검증 계획

- 워커에서 socket send/poll을 호출하지 않고 main이 EPOLLOUT으로 전송하는지 확인.
- 부분 send/EAGAIN 이후 위치 보존, 실제 진행 시 쓰기 기한 갱신, 전송 완료 후 파일/본문 해제 확인.
- 읽지 않는 클라이언트에 보내는 동안 다른 연결 수신·응답 준비·전송이 진행되는지 확인.
- 큐가 포화돼도 503 경로가 워커 큐에 의존하지 않는지, 503 부분 송신 실패/기한 초과 시 정리되는지 확인.
- CLI 기본값 워커4/큐64/연결128/수신32 MiB 및 잘못된 인자 거부 검증 (옵션명: --worker/--queue/--max/--buffer, 세부 범위 미정).
- Docker Linux에서 실행. 종료 시 큐 대기·실행 작업의 응답 전송 완료와 신규 작업 접수 중단을 확인 (전체 종료 기한 없음, 부분 수신/idle close, 기존 SENDING 완료).

이번 정책 반영은 문서만 수정했으며 위 실행 검증은 아직 수행하지 않았다.

- capacity 합계: 연결 간 합산, realloc 실패 시 불변, free 시 차감, 경계값과 예산 초과 close를 검증한다.
- ID 순번을 UINT64_MAX 근처로 주입하여 마지막 값·소진 상태·재사용 금지·저장 실패를 검증한다. 실제 반복으로 소진시키지 않는다.

- 파일 청크 16 KiB 이하, 부분 전송 후 위치 보존, 마지막 청크와 정확한 Content-Length 전송을 검증한다.
- 작업 장벽으로 큐 대기를 길게 유지해도 별도 큐 timeout으로 제거하지 않는지 확인한다. 큐 포화 새 요청의 503 및 기존 I/O 기한과 구분한다.

## 구현 단계 1 실제 결과

macOS에서 test_upload_id, test_http_response, test_fault_recovery, test_transfer_timing, test_path_bounds를 빌드·실행하여 통과했다. ID 경계값/독립 스레드 문맥과 기존 응답·실패 주입 호환성을 확인했다. 소스 include 방식 테스트는 신규 외부 응답 API의 테스트별 이름을 분리했다. Docker daemon 29.7.2와 Ubuntu 22.04 빌드 이미지 준비도 확인했다. epoll 통합 실행은 후속 단계에서 검증한다.

## 구현 단계 2 실제 결과

macOS test_thread_pool 통과: 두 워커가 동시에 시작한 상태에서 두 대기 작업으로 큐를 채우고 추가 제출의 즉시 포화 반환을 확인했다. 장벽 해제 후 승인된 작업 네 개의 완료가 유실되지 않고 회수되는 것도 확인했다.

## 구현 단계 3 실제 결과

Docker Ubuntu22.04/Linux arm64에서 경고 없는 제품 빌드와 tests/tools/test_epoll_server.py 통과를 확인했다. 실제 main으로 느린 헤더·워커 수보다 많은 idle 연결·버퍼 내 pipeline·늦은 GET body 뒤 POST·SHUT_WR·24개 병렬 업로드·연결 상한1의 수락 pause/resume·수신 버퍼1 MiB 초과 close·잘못된 CLI 거부를 검증했다.

큐 포화는 테스트 전용 LD_PRELOAD 읽기 장벽으로 워커1개를 파일 읽기에서 멈추고 큐1개를 채운 상태로 재현했다. 추가 요청의 503/Connection: close·EOF를 확인하고 SIGTERM 후 장벽을 해제하여 256 KiB 파일의 후속 청크와 기존 큐 요청이 모두 전송된 뒤 정상 종료하는 것을 검증했다. 이 장벽 코드는 제품 실행에 포함되지 않는다.

실행: `make docker-test`는 이미지 빌드 후 컨테이너에서 기존 회귀와 실제 main 통합 검증을 실행한다. Docker 이미지의 포트는8000, 정적 루트는/app/www, 업로드 저장소는/app/uploads_storage로 기존 위치를 유지한다. 호스트 실행 예: `docker run --rm -p 8000:8000 jungle-f2`.

기존 회귀 실행 결과: Docker Linux에서 `make test` 전체 통과. root 컨테이너에서 생략되는 권한 거부 사례는 별도 UID1000 컨테이너에서 test_http_response/test_post를 실행해 통과했다. 기존 test_post의 파일명 snprintf 길이 경고는 테스트 코드의 기존 경고이며 제품 빌드 경고는 없었다.

부모 에이전트의 독립 실제main 검증: 워커1개와16 MiB 파일, 수신버퍼1024의 읽지 않는 상대를 둔 동안 다른 GET이2초 이내 완료했고, 6초 후 SIGTERM에서3초 이내 정상 종료했다. 기능 확인의 시간 상한이며 처리량 벤치마크가 아니다.
