# F1. 테스트 케이스

현재 테스트 코드와 기존 실행 기록을 한 표로 통합했다. **통과**는 실제 실행한 범위만 뜻하며, **부분 검증**과 **미실행**은 추가 검증이 필요하다. 이전 계획표의 일괄 미실행 표시는 폐기한다.

자동 테스트는 macOS에서 `make test` 및 ASan/UBSan으로 실행해 통과했다. 2026-11-24 GET·라우팅 보강에서 `make test`와 `build/f1-get-routing-sanitize` 전체 검사를 실행했다. TCP/curl 항목은 POST 구현 당시 별도로 실행한 수동 검증 기록이다.

| 카테고리 | 테스트 코드 | 설명 | 통과여부 |
|---|---|---|---|
| 수신·파싱 | [test_http_request.c](../../tests/test_http_request.c) · `main` | 요청을 한 바이트씩 늘려 전달해 CRLF·헤더 경계가 완성되기 전 NEED_MORE, 전체 POST 수신 후 COMPLETE 확인 | 통과 — 자동 |
| 수신·파싱 | [test_http_request.c](../../tests/test_http_request.c) · `main` | 요청 라인의 메서드·URI 오프셋과 길이 추출 확인 | 통과 — 자동 |
| 수신·파싱 | [test_http_request.c](../../tests/test_http_request.c) · `main` | 헤더·본문·후속 바이트가 함께 있을 때 첫 POST의 소비 범위만 계산 | 통과 — 자동 |
| 수신·파싱 | [test_http_request.c](../../tests/test_http_request.c) · `main` | Content-Length: 4, 본문 00 0D 0A 78의 NUL·CRLF 포함 바이트 처리 | 통과 — 자동 |
| 수신·파싱 | [test_http_request.c](../../tests/test_http_request.c) · `main` | socketpair로 5,000바이트 본문 수신, 초기 4 KiB 이후 버퍼 확장과 필드 유효성 확인 | 통과 — 자동 |
| 수신·파싱 | [test_http_request.c](../../tests/test_http_request.c) · `main` | 불완전 요청 후 shutdown(SHUT_WR)의 EOF를 HTTP_INCOMPLETE_REQUEST로 구분 | 통과 — 자동 |
| 요청 검증 | [test_http_request.c](../../tests/test_http_request.c) · `error` | Host 누락, 비정상 행 구분, 지원하지 않는 HTTP 버전 검출 | 통과 — 자동 |
| 요청 검증 | [test_http_request.c](../../tests/test_http_request.c) · `test_host_policy` | Host B안 정상·거절 입력, 정상 입력 모든 접두 길이, 비정상 요청 라인과 미지원 버전 표본 검증 | 통과 — 자동 |
| 요청 검증 | [test_http_request.c](../../tests/test_http_request.c) · `error` | Content-Length의 음수·비숫자·중복·정수 오버플로우 거절 | 통과 — 자동 |
| 요청 검증 | [test_http_request.c](../../tests/test_http_request.c) · `error` | Transfer-Encoding 단독과 Content-Length 병용 오류 구분 | 통과 — 자동 |
| 요청 검증 | [test_http_request.c](../../tests/test_http_request.c) · `error` | POST Content-Length 누락은 411용 오류, 중복 Content-Type은 400용 오류 | 통과 — 자동 |
| 크기·우선순위 | [test_http_request.c](../../tests/test_http_request.c) · `test_size_priority` | 요청 라인 8 KiB·헤더 32 KiB의 직전·정확한 경계·초과 검증 | 통과 — 자동 |
| 크기·우선순위 | [test_http_request.c](../../tests/test_http_request.c) · `test_size_priority` | 길이 초과와 잘못된 메서드·헤더 이름이 함께 있으면 문법 오류 우선 | 통과 — 자동 |
| 크기·우선순위 | [test_http_request.c](../../tests/test_http_request.c) · `test_size_priority` | 실제 수신에서 헤더 누적량이 32 KiB를 넘지 않음 | 통과 — 자동 |
| 크기·우선순위 | [test_post_boundaries.c](../../tests/test_post_boundaries.c) · `test_body_boundaries` | 본문 상한 초과 413 및 정확히 1 MiB인 users·upload 전체 수신·성공 응답 검증 | 통과 — 자동 |
| 수신 기한 | [test_http_request.c](../../tests/test_http_request.c) · `test_header_timeout` | 입력 없는 연결은 실제 5초 뒤 헤더 타임아웃 | 통과 — 자동 |
| 수신 기한 | [test_http_request.c](../../tests/test_http_request.c) · `test_header_timeout` | 부분 헤더를 20ms마다 보내도 테스트용 200ms 절대 기한이 연장되지 않음 | 통과 — 자동 |
| 수신 기한 | [test_http_request.c](../../tests/test_http_request.c) · `test_header_timeout` | 이미 기한이 만료됐으면 읽을 데이터가 있어도 타임아웃 | 통과 — 자동 |
| 본문 정책 | [test_http_request.c](../../tests/test_http_request.c) · `test_body_policy` | GET은 Content-Length가 있어도 본문 없이 헤더만으로 완료 | 통과 — 자동 |
| 수신 기한 | [test_http_request.c](../../tests/test_http_request.c) · `test_body_policy` | POST 본문을 2초마다 추가해도 헤더 완료부터 전체 10초 뒤 타임아웃 | 통과 — 자동 |
| 수신 기한 | [test_transfer_timing.c](../../tests/test_transfer_timing.c) | 실제 recv 성공 직후 모사 기한 경과: GET 헤더·POST 본문의 완성/미완성 4개 분기 검증 | 통과 — 제어된 I/O·시계 |
| 응답 경계 | [test_http_response.c](../../tests/test_http_response.c) · `test_api_bounds` | SIZE_MAX 오프셋·길이, 잘못된 method/host 범위, 빈 URI, NULL 인자에 500 | 통과 — 자동 |
| 응답 경계 | [test_http_response.c](../../tests/test_http_response.c) · `test_api_bounds` | 오류 응답은 수신 버퍼 없이도 생성, 유효 버퍼 안의 URI 8 KiB 이상은 414 | 통과 — 자동 |
| 응답 경계 | [test_path_bounds.c](../../tests/test_path_bounds.c) | 실제 decode_path에 작은 출력 버퍼를 직접 전달해 종료 문자·index.html 치환 경계·버퍼 밖 sentinel 보존 검증 | 통과 — 자동 |
| GET 정적 파일 | [test_http_response.c](../../tests/test_http_response.c) · `check` | `/`의 index.html과 64 KiB PNG 본문 바이트·Content-Length 일치 | 통과 — 자동 |
| GET 정적 파일 | [test_http_response.c](../../tests/test_http_response.c) · `check` | PNG MIME, 쿼리 제거와 퍼센트 디코딩 확인 | 통과 — 자동 |
| GET 정적 파일 | [test_http_response.c](../../tests/test_http_response.c) · `test_get_routes` | HTML·CSS·JS·JSON·PNG·JPG·JPEG·미등록 확장자 및 대문자 CSS의 MIME·본문 바이트 검증 | 통과 — 자동 |
| GET 정적 파일 | 수동 TCP 검증 · 저장된 전용 테스트 코드 없음 | 기본 페이지 응답 확인 | 통과 — 수동 |
| GET 정적 파일 | [test_http_response.c](../../tests/test_http_response.c) · `test_get_routes` | `/css/main.css?v=1` 실제 하위 디렉토리 파일의 MIME·본문·길이 검증 | 통과 — 자동 |
| GET 경로 보호 | [test_http_response.c](../../tests/test_http_response.c) · `check` | 없는 파일 404, 디렉토리 403, 중간·최종 심볼릭 링크 403 | 통과 — 자동 |
| GET 경로 보호 | [test_http_response.c](../../tests/test_http_response.c) · `check` | 인코딩된 .., %00, 잘못된 %XX 차단 | 통과 — 자동 |
| GET 경로 보호 | [test_http_response.c](../../tests/test_http_response.c) · `test_file_access` | 실제 파일 chmod(0000)의 403, www 루트 누락의 500 | 통과 — macOS 일반 사용자 |
| 오류 응답 | [test_http_response.c](../../tests/test_http_response.c) · 상태 매핑 목록 | A 오류의 400/408/411/413/414/431/500/501/505 변환 | 통과 — 자동 |
| 라우팅 | [test_http_response.c](../../tests/test_http_response.c) · `check` | GET /api/users는 405 + Allow: POST, POST /는 404 | 통과 — 자동 |
| 라우팅 | 수동 TCP 검증 · 저장된 전용 테스트 코드 없음 | 인코딩·쿼리가 있는 예약 API 경로, 알 수 없는 POST 경로 확인 | 통과 — 수동 |
| 라우팅 | [test_http_response.c](../../tests/test_http_response.c) · `test_get_routes` | 예약 API 3종 × 메서드 9종 × 원문/인코딩+쿼리 경로에서 405·Allow: POST 검증 | 통과 — 자동 |
| 전송 | [test_http_response.c](../../tests/test_http_response.c) · `check` | 4 KiB 송신 버퍼에서 16 KiB 청크 기반 바이너리 응답 전체 바이트 일치 | 통과 — 자동 |
| 전송 | [test_http_response.c](../../tests/test_http_response.c) · `main` | 상대가 읽지 않으면 실제 5초 쓰기 타임아웃, O_NONBLOCK 유지 | 통과 — 자동 |
| 전송 | [test_http_response.c](../../tests/test_http_response.c) · `main` | 상대 연결 종료 후 전송 실패 반환 | 통과 — 자동 |
| 전송 | [test_transfer_timing.c](../../tests/test_transfer_timing.c) | 실제 socketpair 부분 송신·상대 drain과 모사 시간으로 양수 send 기한 갱신, EAGAIN 무갱신·최종 만료 검증 | 통과 — 제어된 I/O·시계 |
| 전송 | [test_fault_recovery.c](../../tests/test_fault_recovery.c) | poll/recv/send/read의 EINTR 재시도·I/O 실패 및 헤더/부분 본문 전송 후 read 실패 시 추가 응답 없음 | 통과 — 오류 주입 |
| JSON 파서 | [test_json.c](../../tests/test_json.c) | 객체·배열·숫자·불리언·null 및 한글을 해석·재직렬화, 큰 정수 원문 보존 | 통과 — 자동 |
| JSON 파서 | [test_json.c](../../tests/test_json.c) | 문자열 NUL·surrogate pair 보존, 잘못된 UTF-8·고립 surrogate 거절 | 통과 — 자동 |
| JSON 파서 | [test_json.c](../../tests/test_json.c) · `expect` | 잘못된 숫자·이스케이프·후행 데이터·후행 쉼표·빈 입력 400 | 통과 — 자동 |
| JSON 파서 | [test_json.c](../../tests/test_json.c) · `expect` | 이스케이프 해석 후 동일한 JSON 키 중복 400 | 통과 — 자동 |
| JSON 제한 | [test_json.c](../../tests/test_json.c) | 깊이 16/17, 객체 필드 100/101, 배열 원소 1,000/1,001, 전체 값 10,000/10,001 | 통과 — 자동 |
| 폼 파서 | [test_json.c](../../tests/test_json.c) | %XX·+·한글·NUL 디코딩, 비정상 %XX 및 디코딩 후 중복 이름 400 | 통과 — 자동 |
| 폼 제한 | [test_json.c](../../tests/test_json.c) | 폼 필드 100개 허용·101개 413 | 통과 — 자동 |
| POST 처리 | [test_post.c](../../tests/test_post.c) · `run`/`expect` | /api/users 객체 결과, /api/form 키·값 결과를 JSON으로 재파싱 | 통과 — 자동 |
| POST 처리 | [test_post.c](../../tests/test_post.c) · `expect` | 양수 본문의 타입 누락·미지원 타입·multipart·미지원 매개변수는 415, UTF-8 charset 허용 | 통과 — 자동 |
| POST 처리 | [test_post.c](../../tests/test_post.c) · `expect` | 사용자 API의 배열·잘못된 JSON 400, 알 수 없는 경로 404 | 통과 — 자동 |
| POST 처리 | [test_post.c](../../tests/test_post.c) · `expect` | /api/upload의 명시적 0길이 본문은 200, 파일 생성 없이 data:null | 통과 — 자동 |
| POST 통합 | 수동 TCP 검증 · 저장된 전용 테스트 코드 없음 | 한글·중첩 사용자 JSON, 폼, 411/415 및 0길이 요청 확인 | 통과 — 수동 |
| 업로드 저장 | [test_post.c](../../tests/test_post.c) | 기존 uploads_storage에서 201, 시각 기반 같은 ID의 .bin/.json 쌍 생성 | 통과 — 자동 |
| 업로드 저장 | [test_post.c](../../tests/test_post.c) | 디코딩된 NUL·CRLF·0xFF 바이트, 메타데이터 원본명·크기, 두 파일 권한 0400 확인 | 통과 — 자동 |
| 업로드 저장 | [test_post.c](../../tests/test_post.c) | ../와 한글이 포함된 원본명을 저장 경로로 쓰지 않고 서버 ID 사용 | 통과 — 자동 |
| 업로드 저장 | [test_post.c](../../tests/test_post.c) | 같은 원본명으로 재업로드 시 별도 ID 생성, 기존 파일 유지 | 통과 — 자동 |
| 업로드 저장 | [test_post.c](../../tests/test_post.c) | 디렉토리 누락 시 500, 서버 자동 생성 없음 | 통과 — 자동 |
| 업로드 저장 | [test_post.c](../../tests/test_post.c) | 디렉토리 쓰기 권한 거부 시 500. root로 실행하면 이 케이스는 생략 | 통과 — macOS 일반 사용자 |
| 업로드 Base64 | [test_post.c](../../tests/test_post.c) | 일반 Base64와 필요한 패딩의 정상 디코딩, 잘못된 길이·패딩·pad bits·공백·줄바꿈·URL 알파벳 400 | 통과 — 자동 |
| 업로드 Base64 | [test_post.c](../../tests/test_post.c) | 필수 필드 누락·잘못된 Base64 실패 후 산출물 수가 증가하지 않음 | 통과 — 자동 |
| 업로드 도구 | [upload_fixture.py](../../tests/tools/upload_fixture.py) · 실제 curl 실행 | 한글 파일명과 25,603바이트 원본을 34,247바이트 JSON으로 전송, 원본 바이트·메타데이터·0400 확인 | 통과 — 수동 |
| 업로드 도구 | [test_upload_fixture.py](../../tests/tools/test_upload_fixture.py) | 도구를 별도 프로세스로 실행해 JSON 1 MiB−1·정확 상한 생성 및 +1 사전 거절 검증 | 통과 — 자동 |
| 업로드 저장 | 수동 TCP 검증 · 저장된 전용 테스트 코드 없음 | 0x00~0xFF 전체 바이트 포함 파일의 보존 및 잘못된 Base64의 파일 미생성 | 통과 — 수동 |
| 업로드 저장 | [test_post.c](../../tests/test_post.c) · `test_upload_inputs` | 필드별 비문자열 타입 20개·encoding 오류 7개에 400 및 파일 미생성 | 통과 — 자동 |
| 업로드 Base64 | [test_post.c](../../tests/test_post.c) · `test_upload_inputs` | 패딩 불필요 AAEC 디코딩 및 빈 data의 201·0바이트 파일 저장 | 통과 — 자동 |
| JSON 응답 | [test_post_boundaries.c](../../tests/test_post_boundaries.c) | 따옴표·역슬래시·0x00~0x1F·한글 재파싱 보존, POST 성공·오류의 JSON Content-Type·실제 본문 길이 검증 | 통과 — 자동 |
| 업로드 저장 | [test_post.c](../../tests/test_post.c) · `test_upload_inputs` | photo.jpg·image/png 메타데이터와 다른 00 01 02 원본을 변환 없이 보존 | 통과 — 자동 |
| 업로드 저장 | [test_post.c](../../tests/test_post.c) · `test_upload_inputs` | 절대 경로·기존 파일명·NUL 메타데이터 보존 및 기존 파일 불변 | 통과 — 자동 |
| 업로드 저장 | [test_fault_recovery.c](../../tests/test_fault_recovery.c) | 고정·역행 시각 ID 비중복 및 실제 기존 bin/json 이름 충돌 후 재시도·기존 바이트 보존 | 통과 — 오류 주입 |
| 업로드 저장 | [test_fault_recovery.c](../../tests/test_fault_recovery.c) | bin/json 생성·쓰기·close·fchmod 실패 주입 시 자기 파일 정리, 기존 파일·충돌 파일 보존 | 통과 — 오류 주입 |
| 업로드 저장 | [test_transfer_timing.c](../../tests/test_transfer_timing.c) | 저장 후 첫 헤더·부분 헤더·첫 본문·부분 본문 send 실패 주입: 파일 쌍 보존·추가 응답 없음 | 통과 — 제어된 I/O·시계 |
| 업로드 공개 범위 | [test_http_response.c](../../tests/test_http_response.c) · `test_get_routes` | 실제 저장된 .bin/.json의 직접 GET 404, 원문·인코딩 상위 경로 및 심볼릭 링크 GET 403 | 통과 — 자동 |
| 자원·안정성 | 기존 전체 자동 테스트 + ASan/UBSan | 기존 수신·응답·JSON·POST 테스트에서 메모리 접근·정의되지 않은 동작 오류 없음, GET 분리 후 회귀 통과 | 통과 — macOS |
| 자원·안정성 | [test_stability.c](../../tests/test_stability.c) | 4,400회 FD·RSS 계측 및 Linux LeakSanitizer 통과. 장시간 운용·작은 누수 부재까지 입증하지 않음 | 부분 검증 — 유한 반복·LSan 통과 |
| 자원·안정성 | [test_stability.c](../../tests/test_stability.c) · `test_kill_observation` | SIGKILL 후 부분 bin 2바이트·빈 json 0바이트·0600 잔존 관찰. 파일 쌍 원자성·디스크 동기화는 미보장 | 관찰 완료 — 내구성 미검증 |
| 플랫폼 | [test_stability.c](../../tests/test_stability.c) 및 Linux Docker 전체 검사 | macOS/Linux에서 실제 loopback TCP 2MiB 느린 수신 바이트 일치 및 무수신 전송 실패, Linux ASan/LSan/UBSan 전체 통과 | 통과 — 실제 TCP·Linux |
| 빌드 파이프라인 | [test_smoke.c](../../tests/test_smoke.c) | 테스트 실행 파이프라인 확인. HTTP 기능 검증으로 간주하지 않음 | 통과 — 자동 |

실행 방법:

```bash
make test
make test BUILD=build/get-split-sanitize TBIN=tests/bin/get-split-sanitize \
  EXTRA_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  EXTRA_LDFLAGS='-fsanitize=address,undefined'

# 서버와 기존 uploads_storage를 준비한 뒤 실제 파일을 전송·검증
python3 tests/tools/upload_fixture.py photo.png \
  --url http://localhost:8000/api/upload --content-type image/png
```

macOS 검증에서는 Makefile의 `make asan`에 포함된 LeakSanitizer를 사용하지 않았다. 업로드 도구는 실제 저장을 수행하며 검증 후 파일을 자동 삭제하지 않는다. 과거 수동 TCP 검증 스크립트는 저장소에 없으므로 자동 회귀 테스트와 구분한다.

## GET·라우팅 보강 검증 (2026-11-24)

`test_get_routes`에 73개 요청 검증을 추가했다. 기존 테스트와 동일하게 임시 디렉토리와 socketpair를 사용한다. 실제 TCP 및 Linux 검증은 포함하지 않는다.

| 입력·범위 | 기대 결과 | 실제 결과 |
|---|---|---|
| html/css/js/json/png/jpg/jpeg/unknown/CSS 파일 9종, NUL·CRLF·0xFF 포함 5바이트 본문 | 200, 설계된 MIME, 원본 바이트와 Content-Length 일치 | 통과 |
| `/css/main.css?v=1` | 200, text/css, 하위 디렉토리 파일 바이트 일치 | 통과 |
| users/form/upload × GET/HEAD/PUT/PATCH/DELETE/OPTIONS/TRACE/CONNECT/CUSTOM × 원문 및 `/%61pi/…?v=1` 경로 (54개) | 405, application/json, Allow: POST 한 번 | 통과 |
| PUT /index.html | 405, Allow: GET | 통과 |
| post_prepare로 실제 생성한 .bin/.json × 직접 URL·상위 경로·인코딩 상위 경로·www 안의 저장소 심볼릭 링크 (8개) | 직접 URL 404, 나머지 403; 저장 파일 미제공 | 통과 |

업로드 산출물은 테스트 전용 임시 디렉토리에 생성하고 검증 후 삭제한다. 실행 흐름은 `http_parse_request → http_send_response`이며, 업로드 준비만 `post_prepare`를 직접 호출한다. CONNECT도 기존 파서가 받는 origin-form 경로로 라우팅을 검사하며 CONNECT authority-form 문법은 이번 검증 범위가 아니다.

실행 명령:

```bash
make test
make test BUILD=build/f1-get-routing-sanitize TBIN=tests/bin/f1-get-routing-sanitize \
  EXTRA_CFLAGS='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' \
  EXTRA_LDFLAGS='-fsanitize=address,undefined'
```

두 실행 모두 HTTP request/response, JSON/form, POST, smoke 전체 통과. ASan/UBSan 오류 없음. LeakSanitizer는 포함하지 않았다.

## 카테고리 2·3 병렬 보강 (2026-11-24)

- [카테고리 2 결과](f1_category2_results.md): 업로드 잘못된 입력 27개, 정상 저장·메타데이터·바이트 보존 5개.
- [카테고리 3 결과](f1_category3_results.md): HTTP 교환 26회와 업로드 도구 경계 입력 3개. 정확 상한은 전체 본문을 실제 전송하며, +1은 Content-Length 헤더에서 거절한다. 오류 주입 11개는 상태·응답 헤더 매핑 검사다.
- 두 작업의 일반 전체 및 ASan/UBSan 검사가 통과했다. 통합 후 `make test`에서도 신규 C 테스트와 Python 도구 경계 테스트를 실행해 통과했다. 실제 TCP·Linux·LeakSanitizer 검증은 제외한다.

`make test`는 C 테스트 전체 실행 후 `python3 tests/tools/test_upload_fixture.py`를 실행한다. 상세 입력·기대·실제 결과와 전용 sanitizer 실행 명령은 위 결과 문서에 기록했다.

## 카테고리 4 — Host B안·요청·경로 검증 (2026-11-24)

| 입력 | 기대 결과·검증 방법 | 실제 결과 |
|---|---|---|
| 단일 이름·대소문자·하이픈·마지막 점·IPv4·IPv6·포트 1/65535/선행 0·앞뒤 OWS | COMPLETE, 정상 요청의 모든 접두 길이는 최종 완성 전 NEED_MORE | 통과 |
| 빈 Host·금지 문자·빈 라벨·잘못된 IPv4/IPv6·zone ID·IPvFuture·빈/문자/0/초과 포트·혼합 대소문자 중복 헤더 | BAD_REQUEST (400용 오류) | 통과 |
| 빈/잘못된 메서드, 공백·탭 위치, 빈 구성요소, absolute/authority/asterisk-form, fragment, 잘못된 버전 형태 | BAD_REQUEST; origin-form 기존 정책 유지 | 통과 |
| HTTP/1.0 및 HTTP/2.0 | UNSUPPORTED_VERSION (505용 오류) | 통과 |
| decode_path `/a`의 용량 0/1/2/3, `/%61?ignored` | 부족 시 414, 용량 3이면 `/a`와 NUL; 앞뒤 sentinel 보존 | 통과 |
| `/` 치환 용량 2·sizeof("/index.html")−1·정확 크기 | 부족 시 414, 정확 크기 성공·NUL·sentinel 보존 | 통과 |
| 잘못된 %·%0·%00 및 상위 경로 | 각각 400/400/400/403, sentinel 보존 | 통과 |
| 실제 chmod(0000) 파일 /denied | 403, 일반 사용자(uid 501) 실행으로 생략 없음 | 통과 |
| www를 임시 이름으로 이동 후 GET / | 500, 이후 루트 복구 | 통과 |

최초 실행에서 macOS inet_pton이 `[fe80::1%lo0]`를 허용해 거절 테스트가 실패했다. IPv6 내부 문자 검사를 추가해 zone ID를 명시적으로 차단한 뒤 아래 전체 검사를 다시 실행했다. 임시 파일은 검증 후 정리하며 기존 업로드 파일을 사용하지 않는다. 무한한 비정상 조합 전체를 열거한 것은 아니며 명시된 표본과 경계를 검증한다.

```bash
make test
make test BUILD=build/f1-host-sanitize TBIN=tests/bin/f1-host-sanitize \
  EXTRA_CFLAGS='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' \
  EXTRA_LDFLAGS='-fsanitize=address,undefined'
```

재실행한 일반 전체 및 ASan/UBSan 전체 검사 통과. Python 업로드 도구 경계 검사도 통과했다. 실제 TCP·Linux·LeakSanitizer는 이번 검증 범위가 아니다.

## 카테고리 5 — 기한·전송·저장 후 실패 (2026-11-24)

[상세 입력·기대 결과·실행 기록](f1_category5_results.md)에 11개 시나리오(수신 4·전송 3·저장 후 응답 실패 4)를 기록했다. 실제 제품 소스와 socketpair 바이트·디스크 저장을 사용하고, 단조 시계·poll 대기·부분 send 및 EAGAIN/EPIPE만 테스트에서 제어한다. 기한 경과 후 성공 수신 바이트 반영, 양수 send에서만 5초 갱신, 전송 실패 후 저장 파일 보존과 추가 응답 부재를 확인했다.

단독·일반 전체·ASan/UBSan 전체 테스트 및 Python 도구 경계 검사가 통과했다. 제품 변경은 없다. 실제 TCP·실시간 느린 수신자·Linux·LeakSanitizer 검증과는 구분하며 해당 미실행 항목은 유지한다. 신규 C 테스트는 `make test`에서 자동 실행한다.

## 카테고리 6·7 병렬 검증 (2026-11-24)

- [카테고리 6 상세](f1_category6_results.md): 오류 주입·저장 복구 26개 시나리오. 실제 socketpair·파일을 사용하고 syscall 반환 오류를 제어한다. close 오류는 FD를 실제 닫은 뒤 EIO를 보고하는 모델이다. 단독·macOS 일반 전체·ASan/UBSan 전체 통과.
- [카테고리 7 상세](f1_category7_results.md): 4,400회 요청, FD·RSS 표본, 실제 loopback TCP, SIGKILL 관찰. 최종 동일 소스로 macOS 일반·ASan/UBSan과 Linux 일반·ASan/LSan/UBSan 전체 검사를 통과했다. Linux는 기존 Docker 이미지를 사용했고 LSan 활성 여부는 의도적 17바이트 누수 검출로 확인했다.
- 일반 실행 RSS는 워밍업 이후 최대 증가 0, FD 11개 표본 모두 5였다. ASan RSS는 약 40~41MiB 증가했으며 격리 큐를 감안한 예산 내였다. 이는 유한 실행 결과이며 장시간 누수 부재의 증명이 아니다.
- 실제 TCP 2MiB 전송 성공은 macOS 약 10.4초, Linux 약 13.5초였다. 읽지 않는 상대의 8MiB 전송은 약 5.0~5.4초 뒤 실패했다. 제품 main 전체가 아닌 전용 listener에서 제품 수신·응답 함수를 실행했다.
- 강제 종료 후 부분 bin과 빈 json이 남았다. 이를 원자성·fsync 내구성 통과로 표시하지 않는다. 작은 TCP 버퍼에서의 초기 전송 실패와 최종 조건 조정 이력은 상세 문서에 남겼다.

새 C 테스트는 `make test`에 자동 포함된다. 네 최종 전체 검사 모두 Python 업로드 도구 경계 검사까지 통과했다. Linux에서 기존 test_post의 snprintf 경로에 format-truncation 경고가 있었으며 실행 실패는 없었다. 제품 코드·정책은 변경하지 않았다.
