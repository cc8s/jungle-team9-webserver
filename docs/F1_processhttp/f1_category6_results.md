# F1 카테고리 6 — 오류 주입·저장 복구

## 목적과 범위

`tests/test_fault_recovery.c`에서 실제 수신·GET·POST·응답 소스를 포함해 오류 경계를 제어한다. 제품 코드·API·정책은 변경하지 않았다. 단순 상태 필드 주입이 아니라 실제 처리 루프가 syscall 래퍼의 EINTR/EIO/ECONNRESET/EPIPE/EACCES 결과를 처리하게 한다. 정상 syscall은 실제 socketpair와 임시 파일에 실행한다.

고정 CLOCK_REALTIME은 업로드 ID 생성에만 적용한다. CLOCK_MONOTONIC은 실제 시계를 사용한다. 충돌은 서버가 생성하려는 정확한 이름의 파일을 미리 만들어 실제 O_EXCL이 EEXIST를 반환하게 한다. close 오류는 실제 FD를 닫은 다음 EIO를 반환하는 모델이다. close 실패 시 FD가 열린 채 남는 플랫폼 동작이나 실제 디스크 고장·물리적 신호 전달·TCP 연결 오류 발생 자체를 재현한 검사는 아니다. 삭제 syscall 실패, 전원 장애, fsync 내구성도 이번 범위에 포함하지 않는다.

## 함수 흐름

- 수신: `test_receive_fault → fault_receive_request → wait_for_data/poll → recv → 파싱`. EINTR 후 재시도 완료 또는 읽기 오류 반환을 확인한다.
- 전송: `test_send_fault → fault_send_all → poll/send`. EINTR은 재시도하고 실제 상대 수신 바이트를 확인한다. 실제 I/O 오류로 반환한 경로는 -1과 송신 바이트 0을 검사한다.
- 정적 파일: `test_file_read → fault_send_response → fault_get_prepare → fault_get_send_body → read → fault_send_all`. read를 최대 3바이트로 제한해 첫 읽기/일부 본문 송신 뒤 오류를 확정 재현한다. 이미 200 헤더를 보낸 뒤 추가 HTTP 응답이 붙지 않는지도 확인한다.
- 업로드: `upload_request → fault_post_prepare → upload → store_upload → openat/write/fchmod/close → cleanup`. 응답 JSON 재파싱, 파일 개수, 기존 파일의 keep 바이트, 생성 FD가 닫혔는지 검사한다. 성공 업로드는 원본 00 0D 0A FF와 두 파일의 0400 권한을 확인한 뒤 테스트 파일만 삭제한다.

## 입력·기대 결과·실제 결과

| 입력·상황 | 기대 결과 | 실제 결과 |
|---|---|---|
| 수신 poll EINTR / EIO | EINTR 재시도 후 COMPLETE / HTTP_READ_ERROR | 통과 |
| 수신 recv EINTR / ECONNRESET | EINTR 재시도 후 COMPLETE / HTTP_READ_ERROR | 통과 |
| 전송 poll EINTR / EIO | 재시도 후 정확한 bytes 5바이트 / -1·수신 0바이트 | 통과 |
| 전송 send EINTR / EPIPE | 재시도 후 정확한 bytes 5바이트 / -1·수신 0바이트 | 통과 |
| 파일 첫 read EINTR | 재시도 후 9바이트 abcdefghi 전체 일치 | 통과 |
| 파일 첫 read EIO | -1, 이미 전송된 200 헤더만 존재·추가 응답 없음 | 통과 |
| 파일 3바이트 송신 후 두 번째 read EIO | -1, 본문 abc만 존재·추가 HTTP 응답 없음 | 통과 |
| .bin openat EACCES / .json openat EIO | 500, 이번 생성 파일 쌍 정리, 기존 keep.bin 보존 | 통과 |
| .bin / .json write EIO (각각) | 500, 자기 생성 파일만 정리, 기존 파일 보존 | 통과 |
| .bin / .json write EINTR (각각) | 재시도 후 201, 실제 바이트·권한 정상 | 통과 |
| .bin / .json fchmod EACCES (각각) | 500, 자기 생성 파일만 정리, 기존 파일 보존 | 통과 |
| .bin / .json close EIO (각각, 실제 close 후 오류 반환) | 500, 두 파일 정리·FD 닫힘·기존 파일 보존 | 통과 |
| 기존 .bin 충돌 / 기존 .json 충돌 | EEXIST 후 새 ID로 재시도·201, 기존 충돌 파일 불변; json 충돌 시 임시 bin 제거 | 통과 |
| 각 충돌 재시도 뒤 write EIO | 500, 이번 파일 쌍만 제거, 충돌 파일·keep.bin 보존 | 통과 |
| 같은 시각 2회 업로드 후 시각 1시간 역행 업로드 (각 호출의 sequence는 증가) | 세 ID 모두 다름, 이전 파일 덮어쓰기 없음·원본 바이트 보존 | 통과 |

총 26개 시나리오(수신 4·전송 4·파일 읽기 3·저장 syscall 10·충돌 4·고정/역행 시각 1)다. 파일은 전용 `/tmp/fault-recovery-XXXXXX`에만 생성하고 정리한다.

## 실행 명령과 결과

```bash
make tests/bin/test_fault_recovery
./tests/bin/test_fault_recovery
make test BUILD=build/f1-category6 TBIN=tests/bin/f1-category6
make test BUILD=build/f1-category6-sanitize TBIN=tests/bin/f1-category6-sanitize \
  EXTRA_CFLAGS='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' \
  EXTRA_LDFLAGS='-fsanitize=address,undefined'
```

최초 컴파일에서 테스트 전용 이름으로 바꾼 send 함수의 사전 선언이 누락돼 실패했다. 테스트 파일에 선언을 추가한 후 단독 실행이 통과했다. 제품 수정은 필요하지 않았다.

2026-11-24 macOS에서 단독 실행, 일반 전체 C 테스트와 ASan/UBSan 전체 C 테스트가 통과했다. 두 전체 실행 모두 Python 업로드 도구 경계 검사도 통과했다. ASan/UBSan 오류는 보고되지 않았다. LeakSanitizer·Linux·실제 TCP 검증은 포함하지 않는다.

고정/역행 시계 검사는 정상 호출마다 sequence가 증가하는 상태에서 ID 비중복을 검증한다. 이 자체가 기존 ID 충돌을 일으킨다는 뜻은 아니다. 기존 파일 충돌 검사는 별도로 openat 직전 정확한 대상 이름에 keep 파일을 생성해 실제 EEXIST를 유발한다.
