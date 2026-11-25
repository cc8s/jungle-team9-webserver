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
    handle_connection(conn_fd, accepted_at)
      연결 단위 buffer 생성
      요청마다 request 초기화 → http_receive_request
      http_respond → GET/POST 응답
      종료 여부 판정 → GET 본문 폐기 → memmove
      마지막에 buffer 해제
    close(conn_fd)
```


## 구현 단위

구현·검증·커밋은 다음 두 단위로 나눈다. 표의 항목은 각 단위 내부의 처리 순서이며, 독립적인 구현 단계가 아니다.

| 단위 | 처리 내용 | 완료 기준 |
|---|---|---|
| A: HTTP request 파싱 | 수신 누적 → 요청 라인 → 헤더 → Content-Length와 첫 요청 경계 확인 | 요청 정보 또는 오류를 B에 전달할 수 있고 파서 테스트가 통과함 |
| B: HTTP response 생성·쓰기 | 경로 검증 → 파일 접근 → MIME·길이 결정 → 헤더·본문 전송 → 정리 | GET 정적 응답과 오류 응답 테스트가 통과하고 연결을 닫음 |

POST 본문 수신·API 처리·파일 저장을 연결했다. 아래 POST 호출 흐름을 현재 동작으로 사용한다. 이전 B 호출 흐름의 GET 전용 분기는 확장 전 기록이다.

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
    decode_path → get_prepare (http_get.c)
    send_headers
    get_send_body (http_get.c) → http_send_all 반복
    close(file)
  free(buffer.data)
  close(conn_fd)
```

- `http_send_response`: B의 진입점. 성공 0, 전송 실패 -1을 반환한다.
- `decode_path`: 쿼리를 먼저 분리하고 한 번 디코딩한 경로를 검증한다.
- `open_static`: www부터 디렉토리 FD를 이동하며 파일을 연다. 실패 시 중간 FD를 정리한다.
- `send_headers`/`send_error`: 상태 라인·표준 헤더·오류 본문을 구성한다.
- `http_send_all`: 부분 전송, EINTR/EAGAIN, 진행 없는 5초 제한을 처리한다.
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

## 수신 결과와 응답 경계 보완

```text
wait_for_data(남은 절대 기한)
→ recv → 재시도 오류 / 실제 오류 / EOF 판정 → buffer.length 누적
→ 파서: 헤더 문법 우선 검사 → header_size_error → 본문 완료 판정

http_send_response
→ A 오류이면 버퍼 없이 오류 응답
→ 정상 요청 필드 범위 검사(500) → URI 길이 검사(414)
→ decode_path(..., sizeof(path)) → 기존 파일 응답 흐름
```

헤더 recv에 전달하는 길이는 남은 수신 버퍼 공간과 헤더 누적 상한 중 작은 값이다. 필드 범위 검사는 length <= buffer.length - offset 형태로 덧셈 오버플로우를 피한다.

## POST 확장 호출 흐름

```text
main: accept → 기존 논블로킹 설정
  A: http_receive_request / http_parse_request 확장
    Content-Length 존재 여부와 Content-Type 저장
    헤더 문법·전송 코딩 충돌 검사 → POST 길이 누락이면 오류(411)
    명시적 0이면 본문 대기 없이 완료
    양수이면 기존 본문 1 MiB·10초 제한으로 수신
  B: http_send_response 확장
    A 오류 매핑 → JSON 오류 응답
    필드 범위 검사 → decode_path → 경로·메서드 분기
      GET 정적 경로 → 기존 파일 응답
      예약 API의 비POST → 405 + Allow: POST
      알 수 없는 POST 경로 → 404
      POST 예약 경로 → post_prepare
        길이 0 → 400 + invalid_body (Content-Type 검사·파싱·저장 전 반환)
        Content-Type 검증 → 지원하지 않으면 415
        /api/users → JSON 파싱·객체 검사 → 결과 JSON 생성
        /api/form → 폼 파싱 → 결과 JSON 생성
        /api/upload → JSON 파싱 → 일반 Base64 형식 검증·디코딩
          기존 uploads_storage 접근 → 현재 시각 기반 ID 생성
          <ID>.bin 바이트 기록 + <ID>.json 메타데이터 기록
          두 파일 모두 0400 설정 → FD 정리
          저장 성공 → 201 + 식별자·원본 파일명·크기 JSON
          중간 실패 → 이번 요청의 불완전 파일 쌍 정리 → 오류 응답
    공통 send_headers → http_send_all
  응답·파싱 임시 자원 정리
  기존 main: free(buffer.data) → close(conn_fd)
```

### 역할 분리와 재사용

- post_prepare는 http_post.c에 구현한 POST 진입점이다. 응답 상태와 메모리 JSON 본문을 준비하며 소켓 전송은 B가 맡는다.
- 기존 http_receive_request는 POST 본문 수신·기한 처리를 재사용하고, http_parse_request에 헤더 존재 여부·타입 정보를 추가한다.
- http_send_response는 GET 정적 처리와 POST 처리를 분기한다. 기존 GET의 파일 접근 흐름은 유지한다.
- JSON 파서는 직접 구현해 사용자 JSON과 업로드 JSON에서 공유한다. 폼 파서는 별도이며 결과 JSON 직렬화는 공유한다.
- 업로드 처리는 Base64 디코딩, 고유 파일 생성, 전체 바이트 쓰기, 실패 파일 삭제를 담당한다. open_static으로 업로드 경로를 만들지 않는다.
- send_headers는 상태·MIME·본문 바이트 수와 경로별 Allow를 받도록 확장한다. JSON 결과는 기존 http_send_all로 전송한다.
- send_error는 기존 GET 오류 처리를 유지하면서 POST 오류 JSON을 지원한다. A에서 본문 수신 전에 발생하는 오류도 해당 정책을 연결한다.

### 버퍼와 파일 소유권

수신 본문은 buffer.data + request.header_length부터 request.content_length바이트다. 핸들러는 이를 문자열 종료에 의존하지 않는 길이 기반 입력으로 사용한다.

수신 버퍼는 기존대로 main이 응답 완료까지 유지한다. POST 처리 측은 JSON 파싱 결과, Base64 디코딩 버퍼, 생성한 응답 JSON 버퍼와 업로드 파일 FD를 정리한다. 디코딩 바이트 길이는 별도 관리하며 strlen으로 계산하지 않는다.

uploads_storage는 미리 준비된 디렉토리이며 서버가 시작 시 생성하지 않는다. 현재 시각 기반의 같은 ID로 .bin과 .json 파일 쌍을 저장한다. 디코딩·쓰기 도중 실패하면 자신이 생성한 불완전 파일만 제거한다. 메타데이터 쓰기·권한 변경 실패도 파일 쌍 정리 대상이다. 두 파일의 기록과 0400 설정 완료 이후에는 전송 실패 여부와 파일 생성 실패를 구분한다.

### 클라이언트 테스트 도구 계획

Python 3 테스트 도구가 기존 uploads_storage 테스트 디렉토리를 준비하고 원본 파일을 바이트로 읽어, 필요한 패딩을 포함한 줄바꿈 없는 일반 Base64 문자열로 인코딩해 UTF-8 요청 JSON 파일을 만든다. curl --data-binary @요청파일로 /api/upload에 보낸다. 수신 Content-Length는 curl이 계산하게 한다. 저장 파일을 원본과 바이트 또는 해시로 비교한다. tests/tools/upload_fixture.py가 해당 역할을 수행한다. 서버를 먼저 실행하고 uploads_storage를 준비한 상태에서 사용한다.

### 파서 내부 표현 보완

JSON 문자열은 디코딩된 UTF-8 바이트와 길이를 함께 보관한다. 폼은 이름과 값을 디코딩한 뒤 필드명 중복을 검사하고 중복이면 400을 반환한다. Base64 형식 검증과 디코딩은 파일 생성 전에 완료해 잘못된 입력이 파일을 남기지 않게 한다.

## POST 구현의 실제 함수 연결

```text
http_parse_request → Content-Length 존재 여부·Content-Type·411 판정
http_send_response
  request_status → POST 오류면 json_error
  필드 범위 검사 → decode_path → POST/예약 API/정적 경로 분기
  POST: post_prepare
    content_type → json_parse 또는 form_parse
    /api/users·/api/form: json_write
    /api/upload: upload → base64_decode → store_upload
      UTC ID → openat으로 .bin/.json 예약 → write_file
      metadata JSON → fchmod(0400) → close
    JSON 응답 메모리 준비
  send_json_response → send_headers → http_send_all → 응답 메모리 해제
main → 수신 버퍼 free → 연결 close
```

json_free는 파싱 트리와 디코딩한 문자열을 회수한다. post_prepare는 트리를 해제하며, store_upload는 디코딩 바이트와 분리된 파일 자원을 관리한다. GET/POST의 전송 루프와 5초 진행 제한은 공유한다.

## GET 분리 후 현재 흐름

```text
http_send_response (http_response.c)
  파싱 오류·범위 검사 → 경로 디코딩 → 메서드·API 라우팅
  POST → post_prepare (http_post.c) → send_json_response
  GET 정적 경로
    get_prepare (http_get.c)
      get_mime_type → open_static → fstat
    send_headers (http_response.c)
    get_send_body (http_get.c)
      read(최대 16 KiB) → http_send_all (http_response.c) 반복
    close(response.file)
```

get_response는 열린 파일 FD, MIME, 본문 길이를 전달한다. get_prepare의 path는 검증된 경로이며 내부 탐색에서 수정될 수 있다. get_send_body는 파일 FD를 닫지 않고, 공통 호출자가 성공·실패 모두 정리한다. 공통 전송 루프는 기존 send_all에서 http_send_all로 이름을 바꾸고 GET 모듈에서도 호출할 수 있게 했다.

## GET·라우팅 보강 테스트 흐름 (2026-11-24)

`main → test_get_routes → check_headers` 순서로 실행한다. check_headers는 요청 문자열을 http_parse_request로 파싱한 뒤, socketpair의 서버 FD를 응답 스레드에 전달한다. 응답 스레드는 http_send_response를 실행하고 FD를 닫는다. 수신 측은 EOF까지 읽고 상태·Content-Length·본문 바이트·지정한 MIME과 Allow를 검증한다.

MIME·하위 경로 요청은 get_prepare → get_mime_type/open_static → get_send_body로 흐른다. 예약 API의 비POST 메서드는 경로 디코딩 후 post_is_route에서 JSON 405와 Allow: POST로 분기한다. 업로드 비공개 검증은 post_prepare로 임시 저장소에 실제 파일 쌍을 만들고 GET을 보낸다. www 안에 없는 직접 경로는 404, 상위 경로와 저장소 심볼릭 링크는 403이며, 테스트가 생성한 파일·링크·디렉토리를 정리한다.

## 카테고리 2·3 보강 테스트 흐름 (2026-11-24)

카테고리 2는 test_post의 test_upload_inputs → post_prepare → JSON 파싱·upload → Base64 디코딩·store_upload 흐름에서 입력 거절과 저장 파일·메타데이터를 검증한다. 카테고리 3은 test_post_boundaries의 exchange가 socketpair와 클라이언트 스레드로 http_receive_request → http_send_response를 실행하고 응답 JSON을 재파싱한다. 정확 1 MiB 본문은 전부 송수신하며, 초과 선언은 헤더에서 413으로 거절한다. 수신 오류 필드 주입은 실제 오류 발생 재현과 별도로 응답 매핑만 검증한다.

make test는 신규 C 테스트를 자동 발견해 실행한 뒤 Python 도구 테스트를 실행한다. test_upload_fixture.py → upload_fixture.py subprocess 흐름에서 전체 JSON 크기 1 MiB−1·정확·+1의 생성·거절 결과를 검사한다. 모든 파일 생성은 전용 임시 디렉토리에서 수행하고 정리한다.

## Host B안·카테고리 4 흐름 (2026-11-24)

http_parse_request는 Host 헤더의 앞뒤 OWS를 제거한 후 valid_host를 호출한다. 대괄호 주소는 허용 문자 확인 → inet_pton(AF_INET6), 그 외는 라벨 문법 → 숫자·점 주소의 inet_pton(AF_INET) 순서로 검사한다. 이후 선택적 포트를 누적하며 65535 초과·0을 거절한다. 실패는 HTTP_BAD_REQUEST로 전달되고 기존 응답 매핑에서 400이 된다.

test_host_policy는 정상·거절 Host와 요청 라인 표본을 파싱한다. test_path_bounds는 실제 private decode_path를 작은 버퍼로 호출해 반환값·종료 문자·버퍼 밖 sentinel을 확인한다. test_file_access는 기존 check → http_send_response → get_prepare/open_static 흐름으로 chmod(0000)의 403 및 정적 루트 누락의 500을 검사한다. 요청 라인·경로·파일 접근의 제품 정책은 바꾸지 않았다.

## 카테고리 5 테스트 흐름 (2026-11-24)

test_transfer_timing은 실제 수신·응답 소스를 테스트 번역 단위에 포함해 시계와 I/O 경계만 제어한다. 수신은 실제 recv 성공 → 모사 시간 이동 → buffer.length 누적 → 재파싱 → 완료 또는 재대기 타임아웃으로 흐른다. 전송은 실제 준비 상태 poll·모사 대기 → 부분 send → 양수 결과에서 deadline 갱신을 확인한다. 업로드는 실제 post_prepare/store_upload 완료 → 헤더·본문의 지정 단계에서 send 실패 → 반환 -1 → 실제 수신 바이트·송신 호출 수·남은 파일 쌍과 권한 검증 순서다. 제품 함수와 자원 소유권은 바꾸지 않았다.

## 카테고리 6·7 테스트 흐름 (2026-11-24)

test_fault_recovery는 실제 수신·GET·POST·응답 소스를 테스트 번역 단위에 포함하고 syscall 래퍼로 EINTR·I/O 오류·실제 EEXIST를 주입한다. 수신·전송 재시도, 부분 파일 응답 중단, 업로드 cleanup 이후 기존 파일 내용·신규 파일 개수·FD 닫힘을 확인한다. 고정/역행 CLOCK_REALTIME과 충돌 직전 sentinel 생성은 별도 분기다.

test_stability의 반복 요청은 socketpair → http_receive_request → http_send_response → free/close/생성 파일 삭제 순서로 4,400회 처리하고 FD·RSS를 계측한다. TCP는 포트 0 loopback listener → fork/accept → 제품 수신·응답 함수와 실제 느린 읽기/무수신 상대를 사용한다. 강제 종료는 실제 업로드 write 일부 성공 → SIGKILL → 부모의 잔존 파일 크기·권한 관찰 → 테스트 파일 정리로 흐른다. 제품 main이나 자원 소유권은 변경하지 않았다.

### POST 입력 검증 보강

- `/api/upload → upload`: 필수 문자열 타입 확인 후 content_type 길이가 0이면 400을 반환하며 저장하지 않는다.
- `/api/form → form_parse → form_decode`: 키·값의 %XX 및 + 해석 후 바이트가 NUL이면 400을 반환한다. form_parse가 중간 객체를 해제하고 post_prepare가 invalid_body 응답을 만든다.
- `form_parse`: &로 조각 경계를 찾고 빈 조각이면 즉시 건너뛴다. 비어 있지 않은 조각만 필드 수를 세고 첫 =로 분리한다. =가 없으면 전체를 키, 빈 문자열을 값으로 전달한다. 이후 form_decode로 키를 해석하고 길이가 0이면 400으로 중단한다. 값 디코딩과 중복 키 검사는 유지한다.

## Persistent Connection 실제 흐름

http_connection.c의 handle_connection이 연결을 최대100개 요청까지 반복한다. 유휴 상태의 wait_next_request는 MSG_PEEK로 다음 첫 바이트·EOF를 확인하고, 데이터가 있으면 다음 요청 헤더 기한을 시작한다. 버퍼에 이미 데이터가 있으면 바로 요청 처리에 들어간다.

http_respond는 공통 응답 준비·전송 후 `{io_error, close_connection}`을 반환한다. GET/POST 오류는 모두 close이며 정상 유지 응답은 Connection을 생략한다. 과거 http_send_response 호출은 기존 단일 응답 테스트·호환 호출자를 위해 close 방식으로 유지했다. 새로운 실제 연결 동작은 test_persistent가 Content-Length 기반 수신으로 검증한다.

정상 GET 응답 뒤에는 헤더 consumed를 제거하고 discard_get_body가 버퍼 안의 본문부터 제거한다. 부족한 본문만 제한된 길이로 recv해 다음 요청을 초과 수신·폐기하지 않는다. 10초·EOF·I/O 실패는 두 번째 응답 없이 반환한다. consume은 memmove로 남은 요청을 보존한다.
