# F2. epoll + pthread 함수 흐름 계획

아래 신규 함수·상태·파일명은 제안이며 미구현이다. 이전 연결 전체를 워커에 맡기는 초안을 대체한다.

## 현재

main → accept → handle_connection
→ http_receive_request → wait_for_data → poll / recv / http_parse_request
→ http_respond → GET/POST 처리 → poll / send
→ GET 본문 폐기 → 다음 요청 대기 → buffer 해제 → main의 close

이 전체가 main 스레드에서 실행된다.

## 전환 후 초기화

main
→ socket / bind / listen / SIGPIPE 무시
→ listener O_NONBLOCK
→ thread_pool_init(N, Q) : mutex·condvar·작업/완료 큐·워커 문맥
→ event_loop_init : epoll_create1·eventfd·연결 테이블
→ listener와 완료 알림 FD를 epoll_ctl(ADD)
→ event_loop_run

## 이벤트 루프: 수신부터 작업 전달

epoll_wait(가장 가까운 활성 기한까지)
→ listener 준비: accept_connections (예산 범위에서 accept, EAGAIN이면 반환)
  → 연결 상한 확인 → 수락 시각·generation·connection 생성
  → 연결 O_NONBLOCK → EPOLLIN 등록
→ 연결 준비: connection_read_step
  → recv → buffer 누적 → http_parse_request
  → NEED_MORE: 헤더/본문 상태·기한 보존 후 루프로 복귀
  → COMPLETE: READY → dispatch_request
  → PARSE_ERROR/수신 timeout: 기존 오류 응답용 작업 구성
→ dispatch_request
  → 큐 공간 없으면 합의한 포화 처리 (루프에서 cond_wait 하지 않음)
  → epoll_ctl(DEL), QUEUED 표시, 객체 수명 보장
  → request_queue_push({connection 식별자, request/buffer 참조, force_close})
  → mutex 해제 전후 일관된 규칙으로 cond_signal

루프만 수신 버퍼를 realloc한다. 전달 후에는 완료 회수까지 버퍼를 수정하지 않는다. 오프셋/길이로 저장한 request와 실제 버퍼의 짝을 유지한다. 전달 실패 시 감시·상태를 일관되게 되돌린다.

## 워커: 요청 하나 처리

worker_main(worker_context)
→ mutex lock
→ while(작업 없음 && 종료 아님) cond_wait
→ request_queue_pop → mutex unlock
→ WORKING
→ http_respond(fd, buffer, request, force_close)
  → GET: get_prepare → send_headers → get_send_body
  → POST: post_prepare → JSON/폼/업로드 → send_json_response
  → http_send_all의 poll(POLLOUT)/send는 1차안에서 유지
→ completion_queue_push({connection 식별자, 응답 결과})
→ eventfd write (알림을 모아도 완료 큐 항목은 전부 회수)
→ 다음 작업 대기

워커는 handle_connection을 호출하지 않는다. 다음 요청 대기·recv·consume·close·epoll 재등록도 하지 않는다. 연결당 작업 한 개이므로 응답 순서를 유지한다.

## 이벤트 루프: 완료 회수와 다음 요청

완료 FD 준비 → eventfd read → completion_queue를 비움
→ generation 일치·연결 수명 확인
→ 응답 실패/close/오류 응답/100개 제한이면 connection_close
→ 유지 가능하면 처리한 요청 consumed만 consume
  → GET 본문 잔량: buffer에서 먼저 폐기, 부족분은 DISCARD_GET_BODY
    → EPOLLIN에서 길이만큼만 recv/폐기, 완료 후 다음 요청 진행
  → buffer에 다음 요청 있음: 즉시 파싱, 완료면 재전달
  → buffer 없음: IDLE, 5초 기한, EPOLLIN 재등록
→ 큐 공간이 생겼으면 READY 연결 재전달 시도

connection_close는 이벤트 루프만 실행한다: 감시 해제 → close → buffer/객체 해제 → 연결 수 감소 → 필요 시 listener 재개. 워커에 인계된 객체는 완료까지 닫거나 해제하지 않는다.

## 종료

종료 요청 알림 → listener 해제·수락 중단
→ 합의한 queued/ready/수신/실행 작업 정책 적용
→ 종료 플래그와 cond_broadcast
→ 완료 이벤트를 계속 회수하며 활성 작업 수명 보장
→ 워커 join → 연결/큐/eventfd/epoll/listener 정리

시그널 핸들러에서 mutex·condvar를 직접 조작하지 않는다. 알림 FD를 이용한 안전한 종료 통지 방식을 구현 시 확정한다.
