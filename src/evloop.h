#ifndef EVLOOP_H
#define EVLOOP_H

#include "worker.h"

/* D1 ⑥: worker 하나가 epoll 로 연결 여러 개를 맡는다 (nginx 구조). prefork_run 에 넘기는 work 함수.
 * ⑤ 의 worker_main 과 같은 worker_ctx 를 받고, 같은 일(정적 파일·프록시)을 하며, 같은 테스트를 통과해야 한다.
 *
 * 연결 하나는 상태 기계다:  READ → (정적) WRITE            → READ(keep-alive) 또는 닫기
 *                                  (프록시) CONNECT → SEND → RECV(중계) → 닫기
 * 모든 소켓은 non-blocking 이라 어느 한 연결이 느려도 worker 가 멈추지 않는다.
 * 종료 시그널은 epoll_pwait 안에서만 받는다 (accept 직전에 시그널이 오면 잠들어 버리는 #10 의 틈이 없음).
 * 정상 종료 0 */
int evloop_main(void *ctx);

#endif
