# F1 카테고리 2 — 업로드 입력·바이트 보존 검증

## 목적과 범위

기존 업로드 계약에서 미실행이었던 JSON 필드 타입, encoding 값, 정상 Base64와 빈 data, 원본명·타입의 메타데이터 처리 및 원본 바이트 보존을 자동 검증한다. 서버 동작이나 정책은 변경하지 않았다. 테스트는 `tests/test_post.c`에 추가했다.

## 함수 흐름

`main → test_upload_inputs → run → post_prepare → JSON 파싱 → upload → Base64 디코딩 → store_upload`로 실행한다. 오류 입력은 400과 저장 파일 수 불변을 확인한다. 정상 입력은 `stored_upload`가 응답을 재파싱하고 서버 ID로 생성된 `.bin`의 바이트·크기·0400 권한과 `.json`의 id/filename/content_type/size·0400 권한을 검사한다. JSON 문자열은 길이 기반 비교로 NUL 뒤 바이트까지 확인한다.

테스트는 별도 `/tmp/post-inputs-XXXXXX` 디렉토리에서 실행한다. 각 정상 요청은 서버가 생성한 파일 쌍만 삭제하고, 마지막에 기존 파일 보존용 fixture와 디렉토리를 정리한다. 실제 사용자 업로드 저장소는 사용하지 않는다.

## 입력과 기대 결과·실제 검증

| 입력 | 기대 결과 | 실제 결과 |
|---|---|---|
| filename/content_type/encoding/data 각각에 null·true·123·배열·객체 대입 (20개) | 400, 생성 파일 0개 | 통과 |
| encoding에 빈 문자열·BASE64·base64url·utf-8·끝 공백·base64 뒤 NUL·짧은 base (7개) | 400, 생성 파일 0개 | 통과 |
| 패딩 없는 정상 `AAEC`, filename=photo.jpg, content_type=image/png | 201, 실제 이미지 형식 판정·변환 없이 00 01 02 보존, 메타데이터 보존 | 통과 |
| JSON의 data가 빈 문자열 | 201, 0바이트 .bin 및 size=0 메타데이터 생성 | 통과 |
| filename=`nul\u0000name`, content_type=`x\u0000y` | 201, NUL 포함 문자열 전부 메타데이터·응답에 보존, 서버 ID로 저장 | 통과 |
| 기존 저장소 파일명 existing.bin | 201, 별도 서버 ID의 파일 쌍 생성, 기존 파일 내용 keep 보존 | 통과 |
| 기존 파일을 가리키는 절대 경로 filename | 201, 절대 경로를 메타데이터로 보존, 기존 파일 내용 keep 보존 | 통과 |

정상 5개 케이스 모두 파일 수가 정확히 2개 증가하고 정리 후 원래 개수로 돌아오는지 검사한다. 빈 data를 담은 JSON 요청은 HTTP 본문 자체가 0바이트인 요청과 구분한다. 후자는 기존 테스트대로 파일을 만들지 않고 200을 반환한다.

## 실행 방법과 결과

```bash
make test BUILD=build/f1-category2 TBIN=tests/bin/f1-category2
make test BUILD=build/f1-category2-sanitize TBIN=tests/bin/f1-category2-sanitize \
  EXTRA_CFLAGS='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' \
  EXTRA_LDFLAGS='-fsanitize=address,undefined'
```

2026-11-24 macOS에서 두 명령 모두 HTTP request/response, JSON/form, POST, smoke 전체 통과. 추가한 27개 오류 조합과 5개 정상 저장 케이스 통과. ASan/UBSan 오류 없음. LeakSanitizer·Linux·실제 TCP 전송은 이번 범위에 포함하지 않았다.
