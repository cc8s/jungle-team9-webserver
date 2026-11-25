# F2. epoll + pthread 함수 흐름 계획

상단은 구현 전의 책임 분할 계획이다. 실제 함수명과 구현 완료 흐름은 아래 구현 단계1~3을 따른다. 이전 연결 전체를 워커에 맡기는 초안은 폐기했다.

## F2 이전 흐름

main → accept → handle_connection
→ http_receive_request → wait_for_data → poll / recv / http_parse_request
→ http_respond → GET/POST 처리 → poll / send
→ GET 본문 폐기 → 다음 요청 대기 → buffer 해제 → main의 close

이 전체가 main 스레드에서 실행된다.

## 전환 후 초기화

main
→ CLI 해석·범위 검증: --worker 4 --queue 64 --max 128 --buffer 32 (MiB)
→ socket / bind / listen / SIGPIPE 무시
→ listener O_NONBLOCK
→ thread_pool_init(N, Q) : mutex·condvar·작업/완료 큐·워커 문맥
→ event_loop_init : epoll_create1·eventfd·연결 테이블
→ listener와 완료 알림 FD를 epoll_ctl(ADD)
→ event_loop_run

## 이벤트 루프: 수신부터 작업 전달

epoll_wait(가장 가까운 활성 기한까지)
→ listener 준비: accept_connections (예산 범위에서 accept, EAGAIN이면 반환)
  → 연결 128개 도달 시 listener 감시 중지 → 공간 확보 후 재개
  → 수락 시각·generation·connection 생성
  → 연결 O_NONBLOCK → EPOLLIN 등록
→ 연결 준비: connection_read_step
  → recv → buffer 누적 → http_parse_request
  → NEED_MORE: 헤더/본문 상태·기한 보존 후 루프로 복귀
  → COMPLETE: READY → dispatch_request
  → PARSE_ERROR/수신 timeout: 기존 오류 응답용 작업 구성
→ dispatch_request
  → 큐 64칸이 모두 차면 고정 503 응답 준비 → SENDING/EPOLLOUT → 전송 완료 후 close
    (작업 큐에 재등록하지 않고, 송신 오류/기한 초과 시 정리)
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
→ response_prepare(buffer, request, force_close) (신규 API 제안)
  → GET: 파일·MIME·길이 등 준비
  → POST: JSON/폼/업로드 처리 후 응답 본문 준비
  → 상태·헤더·본문/파일 자원을 응답 객체로 반환
  → 워커는 소켓에 send/poll하지 않음
→ completion_queue_push({connection 식별자, 준비 결과/응답 객체})
→ eventfd write (알림을 모아도 완료 큐 항목은 전부 회수)
→ 다음 작업 대기

워커는 handle_connection을 호출하지 않는다. 다음 요청 대기·recv·consume·close·epoll 재등록도 하지 않는다. 연결당 작업 한 개이므로 응답 순서를 유지한다.

## 이벤트 루프: 완료 회수와 다음 요청

완료 FD 준비 → eventfd read → completion_queue를 비움
→ generation 일치·연결 수명 확인
→ 응답 객체 소유권 회수 → SENDING → EPOLLOUT 등록
→ response_send_step: 부분 send, 전송 위치 보존, EAGAIN이면 루프로 반환
  → 실제 전송 진행만 쓰기 기한 갱신. 송신 오류/기한 초과이면 close
  → 전송 완료 후에만 응답 객체의 파일 FD·메모리 정리
→ close 지정/오류 응답/100개 제한이면 connection_close
→ 전송 완료 후 유지 가능하면 처리한 요청 consumed만 consume
  → GET 본문 잔량: buffer에서 먼저 폐기, 부족분은 DISCARD_GET_BODY
    → EPOLLIN에서 길이만큼만 recv/폐기, 완료 후 다음 요청 진행
  → buffer에 다음 요청 있음: 즉시 파싱, 완료면 재전달
  → buffer 없음: IDLE, 5초 기한, EPOLLIN 재등록
큐 포화 요청은 READY에서 기다리지 않고 503을 전송한다. 수신 버퍼 총 예산 초과 시 해당 연결을 닫는다. 파일 청크 읽기/공급 API는 추가 설계 필요.

connection_close는 이벤트 루프만 실행한다: 감시 해제 → close → buffer/객체 해제 → 연결 수 감소 → 필요 시 listener 재개. 워커에 인계된 객체는 완료까지 닫거나 해제하지 않는다.

## 종료

종료 요청 알림 → listener 해제·수락 중단
→ 큐에 든 작업·실행 중 작업은 응답 준비와 전송까지 계속 진행
→ 부분 수신/idle/미접수 요청 close, 기존 SENDING은 전송 완료 후 close
→ 전체 종료 기한 없음. 송신 진행 없음 5초 제한 유지
→ 종료 플래그와 cond_broadcast
→ 완료 이벤트를 계속 회수하며 활성 작업 수명 보장
→ 워커 join → 연결/큐/eventfd/epoll/listener 정리

시그널 핸들러에서 mutex·condvar를 직접 조작하지 않는다. 알림 FD를 이용한 안전한 종료 통지 방식을 구현 시 확정한다.

수신 버퍼는 모든 연결의 capacity 합계로 계측한다. 확장 전 증가분을 검사하고 성공 시 반영, 해제 시 차감한다. 업로드 ID는 PID_워커번호_uint64_t로컬순번이며 소진 후 재사용하지 않는다.

확정: 파일 청크는 16 KiB를 유지한다. 큐에 들어간 작업에는 별도 대기 기한을 두지 않으며, 큐가 가득 찬 새 요청은 503 후 close한다. 기존 수신/idle/송신 기한과 구분한다.

## 구현 단계 1: 응답 준비 API와 업로드 ID

http_prepare_response는 기존 경로 검증/GET/POST 분기를 수행하되 소켓에 쓰지 않고 헤더·메모리 본문 또는 파일 FD를 http_prepared_response에 보관한다. http_prepare_file_chunk는 워커가 16 KiB 이하를 읽고, http_prepared_dispose가 최종 파일/본문을 해제한다. 기존 http_respond는 이 준비 API와 기존 송신 API를 조합하는 호환 경로로 남긴다.

upload_id_next는 스레드 로컬 문맥으로 PID_워커번호_uint64_t순번을 발급한다. 풀 워커 외의 호출은 별도 높은 번호 범위에서 스레드 번호를 한 번 할당받는다. 순번 소진은 별도 플래그로 표현하여 UINT64_MAX 뒤 wrap하지 않는다.

## 구현 단계 2: 공통 작업 큐와 완료 큐

thread_pool_submit은 mutex 아래 대기 큐만 변경하고 가득 차면 즉시 반환한다. worker_main은 cond_wait로 잠들고 작업을 꺼낸 뒤 잠금을 해제하여 execute를 수행한다. 완료 작업은 별도 할당 없이 job의 next 포인터로 완료 큐에 연결한 후 알림 콜백을 호출한다. 시작 시 일부 pthread_create 실패도 생성된 워커를 깨워 join한다.

## 구현 단계 3: 실제 epoll 경로

main의 CLI 해석 → event_loop_run → 리스너/완료 eventfd 등록 → epoll_wait → receive_request(recv/http_parse_request) → dispatch → execute_job(응답 준비/첫 파일 읽기) → 완료 큐 → collect_completions → start_sending → send_response(send만) 순서다. 파일 청크를 다 보냈으면 submit_read로 파일 읽기 작업을 제출하고, 포화 시 READ_PENDING으로 감시를 해제해 다음 완료 회수에서 재시도한다. 큐에 들어간 후속 작업도 종료 중 유지된다.

close_connection은 워커 소유 상태를 회수한 뒤에만 호출한다. SIGINT/SIGTERM은 플래그+eventfd로 루프를 깨우고 부분 수신/idle 연결을 닫는다. PREPARING/READING/READ_PENDING/SENDING은 drain한 뒤 풀 join·FD 정리로 종료한다.

## recv 종료·실패 분기 보완

receive_request → recv
→ EINTR: 재시도 / EAGAIN·EWOULDBLOCK: 루프로 반환
→ 실제 읽기 오류: close_connection (워커 제출 없음)
→ EOF + 버퍼 비어 있음/IDLE: close_connection (무응답)
→ EOF + 부분 요청: HTTP_INCOMPLETE_REQUEST → dispatch → 기존 오류 응답

완성된 요청은 recv를 더 호출하기 전에 파싱 완료로 dispatch하므로, 상대가 SHUT_WR한 정상 요청도 처리한다. 읽기 오류에서 임의 500을 생성하던 경로는 제거한다.
