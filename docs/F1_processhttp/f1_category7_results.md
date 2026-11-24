# F1 카테고리 7 — 반복 요청·Linux·실제 TCP·강제 종료 관찰

## 목적과 범위

`tests/test_stability.c`에 반복 요청의 자원 계측, 실제 loopback TCP 느린 수신·무수신, 업로드 중 SIGKILL 관찰을 추가했다. 제품 정책·코드·API는 변경하지 않았다. 기존 Docker Linux 개발 이미지(`c06a334046d4`, Ubuntu 22.04/GCC 11.4.0)를 사용했으며 이미지 다운로드·설치는 하지 않았다. 호스트는 macOS arm64, 일반 사용자 uid 501이며 컨테이너는 uid 1000이다.

## 함수와 데이터 흐름

- 반복: `test_repetition → exchange → socketpair → http_receive_request → http_send_response`를 실행한다. 각 연결의 수신 버퍼·FD를 해제하고, 업로드 파일은 각 요청 후 삭제한다. GET 성공·users JSON 성공·잘못된 JSON 400·업로드 201을 한 묶음으로 반복한다.
- TCP: 테스트 전용 listener가 127.0.0.1의 포트 0에 bind하고 fork한 서버가 accept 후 실제 `http_receive_request → http_send_response`를 호출한다. 제품 main은 고정 포트 8000을 사용하므로 이 검사는 제품 main 전체 프로세스를 실행한 검사는 아니다. 시계·poll·send를 모사하지 않는다.
- 강제 종료: 실제 http_post.c를 테스트 번역 단위에 포함하고 공개 함수 이름만 변경한다. 업로드 write 래퍼가 실제 .bin에 2바이트를 쓴 직후 해당 자식에게 SIGKILL을 보낸다. 부모는 자식 종료 신호와 남은 파일 크기·권한을 관찰하고 테스트 파일만 정리한다.

모든 파일은 전용 `/tmp/stability-XXXXXX` 아래에 생성한다. 테스트 서버와 클라이언트는 자체 자식 프로세스 및 OS 자동 할당 포트를 사용한다. 서버 send는 기존 5초 기한, 클라이언트 read는 10초 SO_RCVTIMEO와 수신 전체 60초 상한을 둔다. 다른 사용자 파일·서버·포트는 사용하지 않는다.

## 입력·기대 결과와 판정 기준

| 항목 | 입력·판정 |
|---|---|
| 반복 요청 | 총 4,400회. 처음 400회를 워밍업으로 삼고 이후 400회마다 10번 FD·RSS 표본 수집 |
| FD | 동일 프로세스에서 `/dev/fd` 개수가 워밍업 이후 모든 표본에서 동일해야 함 |
| RSS | macOS task_info resident_size, Linux /proc/self/statm resident 페이지 계측. 워밍업 대비 최대 증가 일반 32MiB 이내, ASan은 격리 큐를 감안해 256MiB 이내 |
| 실제 TCP 느린 수신 | 2MiB 정적 바이너리, 서버 송신/클라이언트 수신 버퍼 요청값 64KiB, 8KiB씩 읽고 50ms 대기. 본문 전체 바이트 일치, 서버 전송 성공, 총 전송시간 5초 초과 |
| 실제 TCP 무수신 | 8MiB 정적 파일, 송수신 버퍼 요청값 4KiB, 클라이언트가 읽지 않음. 서버 전송 실패, 총 시간 5초 이상·60초 미만 |
| SIGKILL 관찰 | 업로드 .bin 첫 실제 write 일부 완료 시 SIGKILL. 부분 파일 상태를 관찰·기록하며 원자성/디스크 동기화의 성공을 요구하지 않음 |

RSS 기준은 이 유한 실행에서 큰 증가를 잡는 예산이지 작은 누수 부재를 증명하는 기준이 아니다. ASan에서 보이는 지속 증가는 할당 격리 큐 영향이 포함되므로 일반 실행의 RSS와 직접 비교해 누수로 판정하지 않는다. Linux LeakSanitizer를 별도로 실행한다. fork 후 `_exit`·SIGKILL하는 자식은 종료 시 LSan 검사를 실행하지 않으며 부모의 정상 반복 요청과 나머지 정상 종료 테스트가 LSan 검사 대상이다.

TCP의 총 시간은 마지막 양수 send 이후의 무진전 시간과 다르다. 무수신이어도 처음에는 커널이 데이터를 받아 전송이 진행할 수 있다. 실제 총 시간을 기록하고, 마지막 성공 이후 정확히 5초라는 내부 기한 갱신 규칙은 카테고리 5의 제어된 시간 테스트로 따로 검증한다.

## 실행 이력과 한계

최초 TCP fixture(512/768KiB)는 macOS 커널의 선행 버퍼링으로 서버 전송이 4.36초에 끝나 `5초 초과` 판정 조건에 미달했다. 이후 1MiB·작은 4/32KiB 수신 버퍼에서는 macOS에서 일부 본문(한 실행은 589,824바이트)만 수신하고 전송이 종료됐으며, Linux에서는 1MiB 전체 전송에 성공했다. 작은 윈도우에서 수신 측의 주기적 읽기와 서버의 실제 전송 진전이 동일하지 않음을 관찰했다. 해당 macOS 실패를 성공으로 기록하지 않는다.

테스트 상수 조정 중 대기시간이 의도보다 길어진 하네스 문제 및 수신 시간 제한 변수 누락 컴파일 오류도 수정했다. 해당 전용 테스트 프로세스·컨테이너만 종료한 뒤 최종 소스로 재실행했다. 최종 fixture는 2MiB·64KiB 버퍼·50ms 대기이며, 아래 결과는 이 동일 소스의 전체 재실행이다.

이 테스트는 유한 4,400회 반복 관찰이다. 수시간/수일 운용, 임의 스케줄링 전체, 전원 장애, fsync 내구성 및 파일 쌍의 원자성을 보장하지 않는다. SIGKILL 경계는 write 래퍼로 결정적으로 선택했으며 임의 시점의 종료를 전수 검사한 것은 아니다.

## 실행 명령

macOS:

```bash
make test BUILD=build/f1-category7 TBIN=tests/bin/f1-category7
make test BUILD=build/f1-category7-sanitize TBIN=tests/bin/f1-category7-sanitize \
  EXTRA_CFLAGS='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' \
  EXTRA_LDFLAGS='-fsanitize=address,undefined'
```

Linux는 저장소를 읽기 전용으로 마운트한 뒤 컨테이너 임시 디렉토리에 src/tests/Makefile만 복사한다. 원본 빌드와 사용자 파일은 수정하지 않는다.

```bash
docker run --rm --network none --user 1000:1000 \
  -v "$PWD:/source:ro" c06a334046d4 sh -ec '
    mkdir /tmp/work
    cp -R /source/src /source/tests /source/Makefile /tmp/work/
    cd /tmp/work
    make test BUILD=build/f1-category7 TBIN=tests/bin/f1-category7
    ASAN_OPTIONS=detect_leaks=1 make test \
      BUILD=build/f1-category7-sanitize TBIN=tests/bin/f1-category7-sanitize \
      EXTRA_CFLAGS="-fsanitize=address,leak,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" \
      EXTRA_LDFLAGS="-fsanitize=address,leak,undefined"
  '
```

LSan 별도 양성 대조군에서 의도적으로 누수한 17바이트를 실제 검출하고 종료 코드 23을 확인했다. 이 대조군은 정상 테스트에 포함하지 않았다.

## 최종 실제 결과 (2026-11-24)

| 실행 | FD (11개 표본) | 워밍업 RSS → 최종 RSS / 최대 증가 | TCP 느린 수신 | TCP 무수신 | 전체 결과 |
|---|---|---|---|---|---|
| macOS 일반 | 모두 5 | 1,458,176 → 1,458,176 / 0바이트 | 2MiB 일치, 성공, 10,362ms | 실패, 5,419ms | 전체 통과 |
| macOS ASan/UBSan | 모두 5 | 22,839,296 → 65,339,392 / 42,500,096바이트 | 2MiB 일치, 성공, 10,392ms | 실패, 5,409ms | 전체 통과, sanitizer 오류 없음 |
| Linux 일반 | 모두 5 | 1,527,808 → 1,527,808 / 0바이트 | 2MiB 일치, 성공, 13,520ms | 실패, 5,011ms | 전체 통과 |
| Linux ASan/LSan/UBSan | 모두 5 | 23,478,272 → 66,691,072 / 43,212,800바이트 | 2MiB 일치, 성공, 13,457ms | 실패, 5,002ms | 전체 통과, sanitizer 오류 없음 |

네 실행 모두 새 오류 복구·기한 테스트를 포함한 전체 C 테스트와 Python 업로드 도구 경계 검사가 통과했다. Linux GCC는 기존 `tests/test_post.c:224`의 snprintf 경로에 format-truncation 경고를 출력했으며, 이번 신규 파일에서는 경고가 없었다.

네 실행의 SIGKILL 관찰 결과는 동일했다: 부분 `.bin` 2바이트와 빈 `.json` 0바이트가 모두 0600으로 남았다. 이는 프로세스 강제 종료에서 불완전한 파일 쌍이 남을 수 있다는 관찰이며, 파일 쌍 원자성·디스크 동기화가 보장됐다는 뜻이 아니다. 성공한 테스트는 부모가 자신의 임시 파일을 정리했다.

실행 로그는 `/tmp/f1-category7-macos-final.log`, `/tmp/f1-category7-macos-sanitize-final.log`, `/tmp/f1-category7-linux-current.log`에 저장했다. LSan 양성 대조군 로그는 `/tmp/f1-category7-lsan-positive.log`다. 로그는 현재 실행 환경의 임시 증거이며 저장소에 포함하지 않았다.
