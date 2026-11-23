# F1. 함수 흐름

## 현재 코드의 흐름

```text
main()
  socket()
  signal(SIGPIPE, SIG_IGN)
  setsockopt()
  bind()                 ← 현재 포트 8000
  listen()
  while (1)
    accept()             ← 블로킹 리스닝 소켓
    기한 기록 → fcntl(conn_fd, F_SETFL, 기존 플래그 | O_NONBLOCK)
    http_receive_request(conn_fd, &buffer, &request)
      http_parse_request(buffer.data, buffer.length, &request)
      NEED_MORE이면 버퍼 확장 및 read 후 반복
    파싱 결과 또는 오류 출력
    http_send_response(conn_fd, &buffer, &request)
    free(buffer.data)
    close(conn_fd)
```

A(수신·파싱)와 B(응답)를 연결했다. 기존 main의 초기화와 반복문은 유지한다.

## 구현 단위

구현·검증·커밋은 다음 두 단위로 나눈다. 표의 항목은 각 단위 내부의 처리 순서이며, 독립적인 구현 단계가 아니다.

| 단위 | 처리 내용 | 완료 기준 |
|---|---|---|
| A: HTTP request 파싱 | 수신 누적 → 요청 라인 → 헤더 → Content-Length와 첫 요청 경계 확인 | 요청 정보 또는 오류를 B에 전달할 수 있고 파서 테스트가 통과함 |
| B: HTTP response 생성·쓰기 | 경로 검증 → 파일 접근 → MIME·길이 결정 → 헤더·본문 전송 → 정리 | GET 정적 응답과 오류 응답 테스트가 통과하고 연결을 닫음 |

POST 처리는 이후 구현한다. 각 단위의 상세 인터페이스와 타임아웃 구현 수단은 그릴링에서 합의한다. A만 완료된 시점에는 정상 HTTP 응답을 기대하지 않는다.

## 전체 흐름 — A/B 및 본문 타임아웃 구현

```text
accept()에서 conn_fd 획득
→ 수신 버퍼에 바이트 누적
→ 파서에 버퍼와 유효 길이 전달
   ├─ 추가 데이터 필요: 수신 반복
   ├─ 오류: 응답 시작 전이면 오류 응답 시도 → 정리
   └─ 완료: 요청 정보와 본문 범위 전달
→ 메서드와 경로 검증
→ 정적 파일 열기 → 파일 크기와 MIME 결정
→ 상태 라인 + 헤더 + 빈 줄 전송
→ 파일 read → 본문 write 반복
→ 파일과 버퍼 정리 → 기존 close(conn_fd)
```

## 데이터와 자원 소유권

- `conn_fd`: 현재 main이 획득하고 닫는다. 소유권을 바꿀 때는 먼저 합의한다.
- 수신 버퍼: 유효 바이트 수를 별도로 관리한다. 본문 길이는 strlen으로 계산하지 않는다.
- 파싱 필드: `http_field`의 offset·length로 보관한다. 사용 시 `buffer.data + offset`으로 접근하며 NUL 종료 문자열로 취급하지 않는다.
- 파일 FD와 파일 전송 버퍼: 해당 단계에서 수명과 정리 위치를 명시한다.
- 파일 전송 중 실패: 이미 전송한 상태 라인을 바꾸려 하지 않고 정리 경로로 이동한다.

## A 함수별 역할

- `http_receive_request()`: 파서를 호출하고 부족하면 4 KiB부터 버퍼를 확장하여 read한다. EINTR는 재시도하고 EOF와 I/O 오류는 구분한다. 연결 FD와 버퍼를 해제하지 않는다.
- `http_parse_request()`: 헤더 완료 전에는 누적 헤더를 검사하고, 완료 후에는 본문 수신 길이만 확인한다. 소켓 I/O와 메모리 할당을 하지 않는다.
- `find_crlf()`, `is_token()`, `header_is()`, `fail()`: 파서 내부의 경계 탐색·문자 검증·헤더 이름 비교·오류 반환 보조 함수다.
- `http_request.header_length`: 본문 시작 위치, `content_length`: 본문 길이, `consumed`: 완성된 첫 요청의 전체 길이다.

## 헤더 기한 처리

```text
accept 성공 → http_now_ms() + 5000으로 header_deadline 계산
→ http_receive_request(..., header_deadline)
  → 파싱 결과가 NEED_MORE이고 헤더 미완료
  → wait_for_data(): 남은 기한으로 poll
  → recv(..., 0) → 누적 → 파서 반복
  → 기한 초과이면 HTTP_HEADER_TIMEOUT
→ 기존 main에서 버퍼 해제와 close
```

헤더 완료 이후 POST는 본문 기한을 적용한 poll/recv 흐름을 사용하고, 다른 메서드는 헤더에서 완료한다. poll의 EINTR와 recv의 일시적인 EAGAIN은 같은 절대 기한으로 재시도한다.

## B 실제 호출 흐름

```text
main
  A: http_receive_request
  B: http_send_response
    request_status → A 오류이면 send_error
    GET 검사 → 그 외 send_error(405)
    decode_path → get_mime_type → open_static → fstat
    send_headers → send_all
    파일 read → send_all 반복
    close(file)
  free(buffer.data)
  close(conn_fd)
```

- `http_send_response`: B의 진입점. 성공 0, 전송 실패 -1을 반환한다.
- `decode_path`: 쿼리를 먼저 분리하고 한 번 디코딩한 경로를 검증한다.
- `open_static`: www부터 디렉토리 FD를 이동하며 파일을 연다. 실패 시 중간 FD를 정리한다.
- `send_headers`/`send_error`: 상태 라인·표준 헤더·오류 본문을 구성한다.
- `send_all`: 부분 전송, EINTR/EAGAIN, 진행 없는 5초 제한을 처리한다.
- `get_mime_type`/`reason`/`request_status`/`file_status`: MIME·상태 문구·오류 변환 보조 함수다.

main의 기존 socket/bind/listen/accept 흐름과 포트 8000은 유지한다. B는 수신 버퍼와 연결 FD의 소유권을 가져가지 않는다.

## 본문 기한과 메서드 분기

```text
http_parse_request
  헤더 검증 → 비POST: consumed = header_length, COMPLETE
            → POST: 본문 부족이면 NEED_MORE
http_receive_request
  POST 헤더 완료·본문 부족 → body_deadline = 현재 + 10000 (한 번만)
  wait_for_data(본문 기한, HTTP_BODY_TIMEOUT) → recv → 파싱 반복
  본문 기한 초과 → HTTP_BODY_TIMEOUT → B의 408
```

wait_for_data는 헤더와 본문이 공유하는 대기 함수다. 호출자가 설정한 논블로킹 conn_fd에서 recv(..., 0)를 사용한다.

## 논블로킹 I/O 계약

main이 accept 성공 후 conn_fd만 O_NONBLOCK으로 설정한다. A는 poll(POLLIN) → recv(..., 0), B는 poll(POLLOUT) → send(..., 0)를 반복한다. EAGAIN이면 남은 기한으로 다시 대기하고, 양수 반환만큼 처리 위치를 이동한다. B의 파일 읽기 버퍼는 16 KiB이며 별도의 1 KiB 송신 제한은 없다.
