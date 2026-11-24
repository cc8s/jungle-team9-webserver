# F1 카테고리 3 — 크기 경계·JSON 응답

## 목적과 범위

기존 본문 전체 1 MiB 상한, POST JSON 응답 및 업로드 도구의 사전 제한을 자동 검증한다. 서버 동작과 정책은 변경하지 않는다. 본문 상한은 디코딩한 파일 크기가 아니라 JSON 표현과 공백을 포함한 전송 본문의 바이트 수다.

## 함수 흐름

`tests/test_post_boundaries.c`의 `exchange`는 별도 클라이언트 스레드에서 요청을 쓰고 응답을 읽는다. 서버 측은 `http_receive_request → http_parse_request → http_send_response → post_prepare → send_json_response`를 실행한다. 응답은 EOF까지 읽고 HTTP 상태, application/json 헤더, 실제 본문 바이트와 Content-Length, JSON 재파싱 결과를 확인한다. 연결은 socketpair를 사용하며 실제 TCP 검증은 포함하지 않는다.

상한 초과는 Content-Length가 1 MiB+1인 헤더만 전송해 본문 수신 전에 413이 반환되는지 확인한다. 정상 상한 요청은 실제 본문 전체를 전송·수신한다. 수신 오류 11개는 정상 POST 수신 후 request.error를 주입하여 JSON 응답 헤더·상태 매핑을 검증한다. 실제 타임아웃·메모리 부족·전송 코딩 등의 발생 조건 재현 테스트는 아니다.

`tests/tools/test_upload_fixture.py`는 임시 원본 파일과 content-type 길이로 JSON 전체 크기를 조절하고, 기존 upload_fixture.py를 별도 Python 프로세스로 실행한다. 네트워크 옵션 없이 생성과 사전 거절만 검증한다.

## 입력·기대 결과·실제 결과

| 입력 | 기대 결과 | 실제 결과 |
|---|---|---|
| /api/users의 유효 JSON 전체 정확히 1,048,576바이트 | 200, 문자열 길이 1,048,565, 전체 JSON 응답 길이 일치 | 통과 |
| /api/users의 Content-Length 1,048,577 | 413 JSON 응답 | 통과 |
| /api/upload의 Base64 JSON+끝 공백 전체 정확히 1,048,576바이트 | 201, 디코딩 파일 크기·전체 NUL 바이트 보존 | 통과 |
| /api/upload의 Content-Length 1,048,577 | 413, 추가 저장 파일 없음 | 통과 |
| 따옴표·역슬래시·0x00~0x1F 전체 제어 문자·한글을 포함한 사용자 문자열 | 200, 응답 재파싱 후 원본 바이트·길이 일치, Content-Length 실제 바이트 일치 | 통과 |
| users/form 성공, upload 명시적 빈 본문 성공 및 상한 업로드 저장 성공 | 200/201, Content-Type: application/json, 유효 JSON과 정확한 길이 | 통과 |
| users 잘못된 JSON, form 잘못된 %XX, upload 필수 필드 누락 | 400, application/json, 유효 JSON과 정확한 길이 | 통과 |
| 없는 POST 경로 및 API 3종의 text/plain 본문 | 404/415, application/json, 유효 JSON과 정확한 길이 | 통과 |
| request.error에 BAD_REQUEST, HEADER_TIMEOUT, BODY_TIMEOUT, LENGTH_REQUIRED, BODY_TOO_LARGE, LINE_TOO_LONG, HEADERS_TOO_LARGE, READ_ERROR, NO_MEMORY, UNSUPPORTED_TRANSFER, UNSUPPORTED_VERSION 주입 | 각각 400/408/408/411/413/414/431/500/500/501/505, application/json, 유효 JSON과 정확한 길이 | 통과 |
| upload_fixture.py 출력 JSON 1 MiB-1 및 정확히 1 MiB | 종료 0, 정확한 출력 크기, Base64 디코딩 후 원본 일치 | 통과 |
| upload_fixture.py 출력 JSON이 1 MiB+1이 되는 입력 | 종료 2, 상한 오류 메시지, 출력 파일 생성 없음 | 통과 |

C 테스트는 26회 HTTP 교환을 수행한다. 테스트 전용 임시 업로드 파일과 디렉토리는 검증 후 삭제하며 기존 사용자 저장소는 사용하지 않는다.

## 실행과 검증

2026-11-24 macOS에서 다음 명령으로 실행했다.

```bash
make test BUILD=build/f1-category3 TBIN=tests/bin/f1-category3
python3 tests/tools/test_upload_fixture.py
make test BUILD=build/f1-category3-sanitize TBIN=tests/bin/f1-category3-sanitize \
  EXTRA_CFLAGS='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' \
  EXTRA_LDFLAGS='-fsanitize=address,undefined'
```

일반 전체 C 테스트, Python 경계 테스트 3개 입력, ASan/UBSan 전체 C 테스트 통과. LeakSanitizer 및 Linux 검증은 포함하지 않았다.
