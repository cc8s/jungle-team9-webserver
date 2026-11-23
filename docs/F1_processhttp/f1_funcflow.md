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
    accept()             ← 클라이언트 연결 FD 획득
    http_receive_request(conn_fd, &buffer, &request)
      http_parse_request(buffer.data, buffer.length, &request)
      NEED_MORE이면 버퍼 확장 및 read 후 반복
    파싱 결과 또는 오류 출력
    TODO B               ← 응답 생성·쓰기 추가 위치
    free(buffer.data)
    close(conn_fd)
```

A(수신·파싱)를 구현했다. 기존 main의 초기화와 반복문은 유지하며 B(응답)는 아직 미구현이다.

## 구현 단위

구현·검증·커밋은 다음 두 단위로 나눈다. 표의 항목은 각 단위 내부의 처리 순서이며, 독립적인 구현 단계가 아니다.

| 단위 | 처리 내용 | 완료 기준 |
|---|---|---|
| A: HTTP request 파싱 | 수신 누적 → 요청 라인 → 헤더 → Content-Length와 첫 요청 경계 확인 | 요청 정보 또는 오류를 B에 전달할 수 있고 파서 테스트가 통과함 |
| B: HTTP response 생성·쓰기 | 경로 검증 → 파일 접근 → MIME·길이 결정 → 헤더·본문 전송 → 정리 | GET 정적 응답과 오류 응답 테스트가 통과하고 연결을 닫음 |

POST 처리는 이후 구현한다. 각 단위의 상세 인터페이스와 타임아웃 구현 수단은 그릴링에서 합의한다. A만 완료된 시점에는 정상 HTTP 응답을 기대하지 않는다.

## 전체 흐름 — A 구현, B 미구현

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
  → wait_for_header(): 남은 기한으로 poll
  → recv(MSG_DONTWAIT) → 누적 → 파서 반복
  → 기한 초과이면 HTTP_HEADER_TIMEOUT
→ 기존 main에서 버퍼 해제와 close
```

헤더 완료 이후에는 기존 본문 read 흐름을 유지한다. poll의 EINTR와 recv의 일시적인 EAGAIN은 같은 절대 기한으로 재시도한다.
