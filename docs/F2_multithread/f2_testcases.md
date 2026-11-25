# F2. epoll + pthread 검증 계획

## 이번 계획 작업의 실제 확인

- main은 handle_connection을 직접 호출하고, 해당 함수는 수신/응답/다음 요청을 반복한다.
- http_receive_request 내부의 poll 대기는 이벤트 루프에서 재사용할 수 없다. http_parse_request는 대기 없는 파서다.
- 파서는 POST만 전체 본문을 기다린다. GET의 COMPLETE는 헤더 완료이므로 전체 본문 완료와 구분해야 한다.
- http_send_all은 poll(POLLOUT)을 사용한다. 이를 재사용하는 워커는 송신 대기 중 점유된다.
- store_upload 공유 sequence는 멀티 스레드 적용 전 변경 필요. Makefile -pthread는 이미 존재.
- uname -s 결과 Darwin. Linux epoll 실행 테스트는 이번 작업에서 하지 않았다.
- 제품 코드·실행 결과는 변경하지 않았다. git diff --check로 문서 공백 오류를 검사한다.

## 단계별 실행 예정

| 단계/상황 | 기대 결과 | 방법 |
|---|---|---|
| 수신 step: 헤더·POST 분할 | 부족 시 즉시 반환, 완성 시 한 번 COMPLETE | socketpair + 논블로킹 호출, 기존 파서 테스트 |
| A는 느린 헤더, B는 완성 요청 | B가 A의 헤더/idle 기한을 기다리지 않고 처리 | Linux 실제 서버에 독립 TCP 연결 |
| N보다 많은 idle 연결 | idle 연결이 워커를 점유하지 않음 | N+개 keep-alive와 새 GET 동시 실행 |
| 상한 MAX_CONNECTIONS | queued/working/idle 포함 상한 준수, 공간 생기면 수락 재개 | 작은 상한으로 연결 수 계측 |
| 작업 큐 포화 | 이벤트 루프가 큐 공간 대기로 멈추지 않음, 정책대로 재개 | 워커 장벽으로 Q를 채움, READY 재전달 확인 |
| 알림 병합·다수 워커 완료 | 완료 건 유실 없이 큐를 모두 비움 | eventfd 한 번 알림에 여러 완료 삽입 |
| 같은 연결에 여러 요청 | 동시 작업 한 개, 응답 순서 보존 | 파이프라이닝 GET/POST, 작업 시작/완료 기록 |
| 후속 요청이 사용자 버퍼에만 존재 | 새 EPOLLIN 없이 다음 요청 처리 | 한 send에 여러 요청, 완료 후 즉시 파싱 확인 |
| GET 본문이 늦게 도착 | 응답은 먼저, 본문만 폐기, 후속 요청 보존 | 분할 GET body + 다음 요청 전송 |
| 데이터+RDHUP, 작업 중 연결 종료 | 완성 요청의 응답 정책 준수, 이중 close/UAF 없음 | shutdown(SHUT_WR), reset, 워커 장벽 |
| FD 번호 재사용·오래된 이벤트 | 이전 연결 이벤트가 새 연결에 적용되지 않음 | 강제 재사용과 generation 검사 |
| 모든 수신/idle 기한 | 바이트마다 연장 안 됨, 이벤트 없어도 만료 처리 | 시계 제어 + 실제 무데이터 연결 |
| 오류·timeout 작업 | 완성 요청이 아니어도 기존 400/408 등 응답, idle EOF는 무응답 | malformed/partial 요청 회귀 |
| 병렬 업로드 | ID 데이터 레이스 없음, 파일쌍·바이트 일치 | 여러 워커 POST와 기존 ID 충돌 주입 |
| 버퍼 예산 초과 | 전체 메모리 상한 정책 준수, 다른 연결 진행 | 다수 연결에서 큰 부분 본문 전송 |
| 초기화 일부 실패·종료 | 워커/큐/FD/객체 회수, 완료 큐 교착 없음 | pthread_create/epoll_ctl 실패 주입, SIGTERM |
| 기존 HTTP 회귀 | F1 응답·기한·파일 정책 유지 | make test + 실제 Linux main 경유 통합 테스트 |

동시성 구현 후 Linux에서 ASan/UBSan과 별도 TSan 빌드로 검사한다. sanitizer 실행 가능 여부와 실제 결과를 기록한다. 처리량·지연·오류 수는 같은 요청·동시성·환경에서 측정한다. 이 표는 실행 전 계획이며 통과 결과가 아니다.
