#ifndef WORKER_H
#define WORKER_H

#include <stddef.h>

#include "request.h"
#include "route.h"

/* 요청 하나를 담을 수 있는 크기: 헤더 상한 + 본문 상한 */
#define CONN_BUF_SIZE (REQ_MAX_HEADER + REQ_MAX_BODY)

enum read_result
{
    READ_OK,      /* 요청 하나가 다 옴 (req 채움) */
    READ_INVALID, /* 잘못된 요청 (req->status 로 오류 응답) */
    READ_CLOSED,  /* 손님이 요청 도중 닫음 또는 읽기 오류 */
    READ_TIMEOUT, /* 마감 시각까지 요청이 다 안 옴 */
    READ_STOPPED  /* 기다리는 중에 종료 시그널 */
};

/* fd 에서 요청 하나를 다 읽을 때까지 읽고 파싱한다.
 * timeout_ms 는 read 한 번이 아니라 "요청 전체" 에 주는 시간: 1바이트씩 찔끔 보내도 마감은 그대로. */
enum read_result read_request(int fd, char *buf, size_t cap, int timeout_ms, struct request *req);

/* prefork_run 에 넘기는 worker 의 일 */
struct worker_ctx
{
    int listenfd;
    const char *root_real;
    int timeout_sec;
    struct route *routes; /* -R 규칙. fork 뒤 worker 마다 자기 복사본을 가지므로 RR 카운터도 따로 돈다 (D4) */
    int nroutes;
};

/* accept → 요청 읽기 → 응답 → close 를 종료 시그널까지 반복. 정상 종료 0 */
int worker_main(void *ctx);

#endif
