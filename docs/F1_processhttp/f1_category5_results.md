# F1 카테고리 5 — 수신 기한·전송 진전·저장 후 응답 실패

## 목적과 범위

기존 확정 정책 세 가지를 `tests/test_transfer_timing.c`에서 재현한다. 제품 코드·API·정책은 변경하지 않았다. 실제 요청 파싱·수신 루프·전송 루프·업로드 저장·응답 함수를 실행하며, 재현에 필요한 시계와 I/O 경계만 제어한다.

테스트 번역 단위에 실제 `http_request.c`와 `http_response.c`를 포함하고 공개 함수 이름을 테스트 전용으로 바꿔 기존 라이브러리와 링크 충돌을 피한다. 단조 시계는 모사 시간으로 교체하고, poll은 실제 준비 상태를 확인하되 대기 시간을 모사한다. recv와 성공한 send는 실제 socketpair의 바이트를 사용한다. 수신 성공 후의 시간 경과, 부분 송신 크기, EAGAIN/EPIPE는 테스트 래퍼가 지정한다. 테스트 시간을 실제 12초 기다리거나 OS 스케줄링 우연에 의존하지 않는다.

이 검사는 실제 TCP·실시간 느린 수신자의 성능이나 커널 송신 버퍼가 가득 차는 상황을 검증한 것은 아니다. 기존 실제 5초 무진전 검사와 별도로, 기한 갱신의 정확한 시점과 실패 이후 처리 흐름을 결정적으로 검증한다. 기존 플랫폼/실제 TCP 느린 수신자 항목은 미실행으로 남긴다.

## 함수와 데이터 흐름

- 수신: `test_receive_crossing → timing_receive_request(실제 http_receive_request) → http_parse_request → wait_for_data → controlled_poll → controlled_recv`. 실제 recv가 성공한 직후 모사 시간을 기한 뒤로 이동한다. 반환한 바이트가 buffer.length와 데이터에 반영됐는지, 다음 파싱에서 완료 또는 재대기 타임아웃으로 갈리는지 확인한다.
- 전송: `test_send_progress → timing_send_all(실제 http_send_all) → controlled_poll → controlled_send`. 작은 실제 송신과 상대의 drain을 반복하고 최종 deadline, 총 모사 시간, 실제 수신 바이트를 검사한다. send가 EAGAIN이면 기한이 연장되지 않는지도 확인한다.
- 업로드: `test_upload_send_failure → timing_parse_request → timing_send_response → post_prepare → store_upload → send_json_response → timing_send_all`. 저장 후 EPIPE를 주입한다. 응답 함수 반환값·송신 호출 횟수·실제 수신 접두 바이트를 확인하고, 디스크에 남은 `.bin` 원본과 `.json` 메타데이터·0400 권한을 검증한 뒤 테스트 파일만 삭제한다.

## 입력·기대 결과·실제 결과

| 입력·상황 | 기대 결과 | 실제 결과 |
|---|---|---|
| GET 헤더의 마지막 recv 성공 직후 100ms 기한을 지나 101ms로 이동 | 받은 바이트 반영 후 COMPLETE; 소비 범위 일치 | 통과 |
| GET 헤더 1바이트가 남은 recv 직후 101ms로 이동 | 성공 바이트는 보존하고 재대기에서 HEADER_TIMEOUT | 통과 |
| POST 헤더를 미리 준비하고 마지막 본문 recv 후 본문 기한 10000ms를 지나 10001ms로 이동 | 본문 바이트 반영 후 COMPLETE | 통과 |
| POST 본문 1바이트가 남은 recv 후 10001ms로 이동 | 성공 본문 바이트는 보존하고 재대기에서 BODY_TIMEOUT | 통과 |
| 상대가 앞선 바이트를 읽어가며 4000ms마다 3바이트씩 송신, 총 9바이트 | 시작 기한 5000ms를 넘어 총 12000ms까지 성공; 성공 때마다 갱신해 최종 기한 17000ms; 원본 바이트 일치 | 통과 |
| 1000ms씩 경과하며 모든 send가 EAGAIN | 기한은 5000ms로 고정, 5000ms에서 실패, 성공 바이트 0 | 통과 |
| 1000ms에 첫 1바이트 성공 후 나머지는 EAGAIN | 마지막 성공 기준 기한 6000ms, 6000ms에서 실패, 실제 상대 수신은 첫 1바이트만 | 통과 |
| 업로드 저장 후 첫 헤더 send가 EPIPE | 반환 -1, 송신 시도 1회, 수신 바이트 0, 저장 파일 쌍 보존 | 통과 |
| 업로드 201 헤더 19바이트 송신 후 EPIPE | 반환 -1, 송신 시도 2회, 201 접두만 수신, 추가 500/두 번째 응답 없음, 파일 보존 | 통과 |
| 업로드 201 헤더 전부 송신 후 첫 본문 send가 EPIPE | 반환 -1, 송신 시도 2회, 헤더만 수신, 추가 응답 없음, 파일 보존 | 통과 |
| 업로드 201 헤더와 JSON 본문 7바이트 송신 후 EPIPE | 반환 -1, 송신 시도 3회, JSON 접두만 수신, 추가 응답 없음, 파일 보존 | 통과 |

총 11개 시나리오다. 업로드 실패 4개 모두 `.bin`의 00 0D 0A FF 원본 4바이트, `.json`의 filename과 size, 두 파일의 0400 권한을 확인한다. 임시 `/tmp/transfer-timing-XXXXXX/uploads_storage`만 사용하고 검증 후 정리한다.

## 실행 명령과 결과

```bash
make tests/bin/test_transfer_timing
./tests/bin/test_transfer_timing
make test BUILD=build/f1-category5 TBIN=tests/bin/f1-category5
make test BUILD=build/f1-category5-sanitize TBIN=tests/bin/f1-category5-sanitize \
  EXTRA_CFLAGS='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' \
  EXTRA_LDFLAGS='-fsanitize=address,undefined'
```

2026-11-24 macOS에서 단독 최초 실행은 통과했다. 이후 부분 JSON 본문 전송 실패 케이스를 추가한 단독 재실행도 통과했다. 일반 전체와 ASan/UBSan 전체 검사가 통과했으며 Python 업로드 도구 경계 검사도 통과했다. 메모리 접근·정의되지 않은 동작 오류는 보고되지 않았다. LeakSanitizer·Linux·실제 TCP 검증은 포함하지 않았다.
