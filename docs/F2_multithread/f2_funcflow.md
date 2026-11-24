# F2. 함수 흐름 초안

아래 신규 함수명과 파일명은 제안이며 아직 구현하지 않았다.

## 기존 흐름

main (src/main.c)
→ socket / SIGPIPE 무시 / setsockopt / bind / listen
→ accept → http_now_ms → fcntl(O_NONBLOCK)
→ handle_connection (src/http_connection.c)
  → http_receive_request → http_respond
  → 다음 요청 대기·버퍼 소비·반복
  → buffer 해제
→ main에서 close → 다음 accept

## 제안 흐름

main
→ 기존 소켓 초기화
→ thread_pool_init(pool, N, Q)
  → 큐·mutex·condition variable 초기화
  → 워커별 고유 번호와 문맥 준비
  → pthread_create(worker_main, worker_context)
→ accept 반복
  → accepted_at = http_now_ms() (accept 직후 기록 유지)
  → fcntl(O_NONBLOCK)
  → thread_pool_submit(pool, {conn_fd, accepted_at})
    → 잠금 → 종료/포화 검사 → 큐에 값 복사 → 대기 워커 통지 → 잠금 해제
  → 전달 실패 시 main이 close (포화 시 동작은 합의 필요)
  → 다음 accept

worker_main(context)
→ 워커별 업로드 ID 상태 준비
→ connection_queue_pop(pool, &job)
  → 잠금 → 큐가 비었으면 조건 대기 → 작업 꺼내기 → 잠금 해제
→ 합의한 대기 기한 정책 적용
→ handle_connection(job.conn_fd, job.accepted_at)
  → http_buffer 생성 (워커의 연결 전용)
  → http_receive_request
  → http_respond
    → GET: get_prepare → 공통 헤더 전송 → get_send_body
    → POST: post_prepare → JSON/폼/업로드 처리 → 공통 응답 전송
      → store_upload: 워커별 ID 상태 사용, O_EXCL 예약·저장
  → 같은 연결의 다음 요청 반복
  → http_buffer 해제 후 반환
→ close(job.conn_fd)
→ 다음 connection_queue_pop

종료 요청
→ main의 accept 대기를 깨우는 안전한 방식 적용 (구체 방식 미결정)
→ thread_pool_stop: 큐 종료 상태 갱신·대기 워커 broadcast
→ 합의한 대기/활성 연결 정책 수행
→ pthread_join → 동기화 자원·큐·리스닝 소켓 정리

## 책임과 데이터 수명

| 위치 | 책임·수명 |
|---|---|
| main.c | 수락·accepted_at·논블로킹 설정·소유권 전달. 전달 성공 후 해당 conn_fd에 접근하지 않음 |
| thread_pool.c/.h (신규 제안) | 제한 큐·워커 생성/회수·종료 상태. pool과 worker_context는 join 완료까지 생존 |
| worker_main | 연결 처리와 최종 close. handle_connection의 호출자 close 계약을 유지 |
| http_connection.c | 연결 내부 HTTP 순서·기한·버퍼 수명 유지. 풀 잠금에 의존하지 않음 |
| http_post.c | ID 공유 순번 제거. 파일 I/O 전체에 전역 잠금을 걸지 않음 |

condition wait는 잠금을 놓고 잠들며 깨어난 후 다시 획득한다. 따라서 큐 비어 있음·종료 상태를 while로 검사한다. 종료 시에는 모든 대기 워커를 깨워야 한다. 시그널 핸들러에서 mutex/condition 함수를 직접 호출하지 않는다.
