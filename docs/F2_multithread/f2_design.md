# F2. epoll + pthread 전환 계획

## 목적과 현재 상태

사용자 요청: 앞단에서 여러 연결을 감시하고 연결 수를 제한하며, 처리 가능한 요청만 공통 작업 큐에 넣어 고정 워커가 처리한다. 이번 작업은 계획 작성이며 코드 구현은 하지 않는다. 이전 연결 단위 풀 초안은 아래 요청 단위 구조로 대체한다.

현재 main → handle_connection → http_receive_request / http_respond가 같은 스레드에서 끝까지 실행된다. 수신 함수의 poll과 keep-alive 대기가 다음 accept를 지연시킨다. http_parse_request는 소켓을 기다리지 않는 파서이므로 재사용할 수 있다.

## 확정한 방향과 제안 구분

사용자가 선택한 방향:
- 앞단 epoll, 뒷단 pthread 워커 풀.
- 연결 수 제한과 수신 완료 요청의 공통 작업 큐.
- 빈 워커가 작업을 꺼내 처리.

세부 구현 제안 (아래 합의 필요):
- main 스레드 하나가 epoll 이벤트 루프와 연결 상태를 관리한다. N개 워커를 추가한다.
- 리스닝 소켓도 O_NONBLOCK으로 설정하고 epoll에 등록한다. 연결 소켓 역시 O_NONBLOCK이다.
- 처음에는 level-triggered 방식. 이벤트당 수신/수락 예산을 두어 한 연결이 루프를 독점하지 않게 한다.
- 워커는 기존 http_respond로 요청 처리와 응답 전송까지 담당한다. 기존 송신 poll을 우선 재사용하므로 느린 수신자는 워커를 점유한다. 송신까지 이벤트 루프로 옮기는 방식은 별도 확장안이며, 이 경계를 구현 전에 합의한다.
- 정상 요청은 파서 HTTP_COMPLETE 시 큐에 넣는다. 오류·수신 타임아웃은 기존 오류 응답을 위한 별도 오류 작업도 큐에 넣어야 한다. 조용히 종료하던 idle EOF/timeout은 응답 없이 정리한다.

## epoll과 제한의 역할

epoll은 FD의 I/O 준비를 알려주며 요청 완료·연결 수 제한을 판단하지 않는다. 준비 이벤트 → recv → 버퍼 누적 → http_parse_request 순서가 필요하다.

MAX_CONNECTIONS는 수락 후 아직 닫지 않은 모든 연결(RECV/READY/QUEUED/WORKING/IDLE/DISCARD 포함)을 센다. 커널 listen backlog, 연결 수, 작업 큐 Q, 워커 수 N은 별개다. 상한 도달 시 listener 감시 중단 후 공간이 생기면 재등록하는 안과 추가 accept 후 close하는 안 중 선택한다. 전자는 커널 대기열에서 연결을 기다리게 한다.

## 연결 상태와 소유권

connection에 fd, 재사용 방지용 generation, 수신 buffer/request, 요청 처리 횟수, 상태, 기한, GET 폐기 잔량을 둔다.

| 상태 | 담당 | 의미 |
|---|---|---|
| RECV_HEADERS / RECV_BODY | 이벤트 루프 | recv·파싱, 부족하면 다음 이벤트까지 반환 |
| READY | 이벤트 루프 | 요청 완료, 큐 포화로 아직 전달하지 못함 |
| QUEUED | 풀 | 작업 큐에 등록, 이벤트 루프는 버퍼·FD를 수정하지 않음 |
| WORKING | 워커 하나 | http_respond 수행, 동일 연결 추가 작업 금지 |
| DISCARD_GET_BODY | 이벤트 루프 | GET 응답 후 Content-Length만큼 본문 폐기 |
| IDLE | 이벤트 루프 | keep-alive 다음 요청 대기, 워커 점유 없음 |
| CLOSING | 이벤트 루프 | epoll 해제·close·버퍼 해제·연결 수 감소 |

전달 시 epoll 감시를 DEL하여 워커와 루프가 같은 FD를 읽거나 닫지 않게 한다. 이미 반환된 이벤트도 상태·generation으로 무시한다. 워커 완료 전에는 루프가 연결 객체를 해제하거나 fd를 닫지 않는다. 오류/종료 통지는 기록만 하고 완료 후 정리한다. 같은 연결은 항상 한 요청만 실행하여 응답 순서를 보존한다.

작업 큐는 mutex + condition variable로 보호한다. 상태변수만 바꾸면 워커가 깨어나지 않는다. 큐 삽입 후 signal, 워커는 while(큐 비었고 종료 아님) cond_wait 후 pop한다. 큐와 알림을 같은 동기화 규칙으로 관리한다.

워커 → 완료 큐(mutex 보호) → eventfd 알림 → 이벤트 루프가 완료 큐를 비우는 구조를 제안한다. eventfd 카운터가 완료 작업 자체는 아니다. 완료 큐 용량을 활성 작업 수에 맞춰 보장하고 유실·교착을 막는다. 작업 완료/큐 공간 확보 시 READY 작업을 재시도하므로 새 EPOLLIN 없이도 재개된다.

## 기존 HTTP 계약 유지

- 첫 헤더: accept부터 5초. 이후 요청: 첫 바이트/버퍼의 다음 요청 처리 시작부터 5초.
- POST: 헤더 완료부터 본문 전체 10초. 기한은 연결 상태에 저장하여 이벤트마다 초기화하지 않는다.
- GET: 기존처럼 헤더 완료 즉시 작업 가능. 본문 전체를 기다리도록 임의 변경하지 않는다. 응답 후 버퍼의 GET 본문을 소비하고 부족분은 DISCARD 상태에서 10초 기한으로 폐기한다. 뒤 요청 바이트는 보존한다.
- keep-alive idle 5초와 연결당 최대 100개 유지. 큐 대기는 idle/수신 시간과 구분하며 별도 처리 대기 기한은 미결정이다.
- 한 번 recv한 데이터에 후속 요청이 있으면 응답 완료 후 버퍼부터 재파싱한다. 커널 이벤트만 기다리면 이미 사용자 버퍼에 있는 요청이 멈출 수 있다.
- EOF/RDHUP와 데이터가 함께 오면 읽은 요청을 판정한다. 쓰기 반쪽 종료는 무조건 응답 취소하지 않는다. 불완전 요청/실제 I/O 오류는 기존 계약에 맞춰 분류한다.
- 이벤트 루프는 연결별 poll을 호출하지 않는다. 가장 가까운 활성 수신/idle 기한으로 epoll_wait timeout을 정하고, 무이벤트 때도 만료 연결을 처리한다.
- 요청·헤더·본문 크기 제한은 유지한다. 연결 상한 외에도 전체 버퍼 예산이 필요하다. 연결마다 최대 약 1 MiB를 쌓으면 총 메모리가 커진다.

## 기존 코드별 변경 계획

| 파일 | 변경 |
|---|---|
| main.c | 직접 handle_connection 호출 제거, 리스너 논블로킹·이벤트 루프·풀 초기화/종료 연결 |
| http_connection.c/.h | 기존 blocking 수명 루프에서 연결 상태와 재개 가능한 수신/소비/폐기 함수 분리. 기존 호출자는 호환 경로 유지 여부 결정 |
| http_request.c/.h | http_parse_request 재사용. http_receive_request의 버퍼 확장·recv 부분을 대기 없는 step으로 추출. 기존 테스트용 API는 우선 유지 |
| event_loop.c/.h (제안) | epoll 등록·수락·기한 검사·상태 전이·완료 회수·최종 close |
| thread_pool.c/.h (제안) | 제한 작업/완료 큐, cond_wait/signal, 워커 생성·join |
| http_response.c | 1차는 http_respond/http_send_all 유지. 송신 이벤트화 선택 시 준비/부분송신 API로 별도 분리 필요 |
| http_post.c | static sequence++ 제거. 기존 선택인 PID + 워커 번호 + 로컬 순번으로 변경, O_EXCL 재시도 유지 |
| Makefile | -pthread는 이미 존재. Linux 전용 실행 검증 경로 추가 |

## 구현 순서와 단계별 완료 기준

1. **정책·환경 확정:** 아래 미결정 항목 합의, Linux 빌드/실행 경로 확보. 이 단계에서는 제품 코드 변경 없음.
2. **수신 step 분리:** poll 없이 recv/파싱 후 NEED_MORE로 반환. 분할 헤더·POST·기한·GET 본문 계약 회귀 확인. 기존 직렬 경로로도 호출 가능하게 분리해 검증.
3. **단일 이벤트 루프:** 연결 객체·리스너·다중 수신·시간 제한·연결 상한 구현. 응답은 테스트 대역으로 확인하여 실제 파일/응답 작업이 루프를 막지 않게 함.
4. **풀·인계·회수:** bounded 작업 큐·완료 큐·condvar·eventfd, generation, FD 단일 소유권 구현. 대역 워커로 포화·중복 전달·종료/실패 정리 검증.
5. **실제 HTTP 연결:** 업로드 ID 경쟁 제거 후 http_respond 연결. 응답 완료 → GET 본문 폐기/버퍼 재파싱/idle 복귀까지 통합. 병렬 GET/POST·파이프라이닝 검증.
6. **운영 경계:** 합의한 큐 포화·메모리 예산·종료 정책 적용, 실패 주입과 Linux sanitizer/부하 검증. 미완료 응답·FD 재사용·완료 알림 집중 검증.

각 단계의 설계·함수 흐름·검증 결과를 갱신하고 해당 작업만 커밋한다. 사용자 작업 중인 F1 문서·phase.md·http_connection.c 변경은 보존한다.

## 구현 전 미결정 사항

1. Linux 전용 epoll로 진행할지, macOS용 kqueue 백엔드도 제공할지. 현재 호스트는 Darwin이며 epoll 직접 실행 불가.
2. 워커가 응답 전송까지 맡는 1차안인지, 워커는 응답 준비만 하고 이벤트 루프가 EPOLLOUT으로 전송할지.
3. N/Q/MAX_CONNECTIONS, backlog, 전체 수신 버퍼 예산 및 설정 방식.
4. 연결 상한 시 listener 일시 중지/새 연결 close, 큐 포화 시 READY 제한 대기/close/503 및 대기 기한.
5. 종료 시 대기 작업 배출/폐기, 실행 중 작업의 종료 기한과 중단 방식. 루프는 워커가 끝나고 완료 큐를 회수할 때까지 살아 있어야 함.
6. 업로드 ID 형식·순번 소진·비워커 테스트 호출의 ID 문맥.

## 근거

- epoll은 준비 이벤트 감시, LT/ET, ONESHOT 재무장 등을 제공한다: https://man7.org/linux/man-pages/man7/epoll.7.html
- eventfd는 epoll로 감시 가능한 스레드 간 알림 FD다: https://man7.org/linux/man-pages/man2/eventfd.2.html
