#include "worker.h"

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "proxy.h"
#include "response.h"
#include "signals.h"
#include "static.h"

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); /* 시계를 바꿔도 거꾸로 가지 않는 시계 */
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

enum read_result read_request(int fd, char *buf, size_t cap, int timeout_ms, struct request *req)
{
    long deadline = now_ms() + timeout_ms;
    size_t len = 0;

    for (;;)
    {
        long left = deadline - now_ms();
        if (left <= 0)
            return READ_TIMEOUT;

        /* 남은 시간만큼만 기다린다. SO_RCVTIMEO 는 read 마다 시간을 새로 세서 찔끔 보내기를 못 막는다 */
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int r = poll(&p, 1, (int)left);
        if (r == 0)
            return READ_TIMEOUT;
        if (r < 0)
        {
            if (errno != EINTR)
                return READ_CLOSED;
            if (stop_requested()) /* 덜 온 요청 + 종료 시그널 → 버린다 (#10) */
                return READ_STOPPED;
            continue;
        }

        ssize_t n = read(fd, buf + len, cap - len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) /* 0 = 손님이 닫음 (EOF) */
            return READ_CLOSED;
        len += (size_t)n;

        /* 바이트가 올 때마다 처음부터 다시 파싱 (파서가 상태를 저장하지 않으므로) */
        enum parse_result pr = parse_request(buf, len, req);
        if (pr == PARSE_OK)
            return READ_OK;
        if (pr == PARSE_ERROR)
            return READ_INVALID;
    }
}

/* 연결 하나: 요청 하나 읽고 응답 하나 보내고 닫는다 (⑤ 는 항상 Connection: close, D5) */
static void handle_conn(int connfd, char *buf, const char *client_ip, struct worker_ctx *w)
{
    struct request req;
    char out[1024];
    pid_t me = getpid();

    /* 응답을 안 받아 가는 손님이 worker 를 붙잡지 못하게: write 한 번이 이 시간 넘게 막히면 실패 */
    struct timeval tv = {.tv_sec = w->timeout_sec, .tv_usec = 0};
    setsockopt(connfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    switch (read_request(connfd, buf, CONN_BUF_SIZE, w->timeout_sec * 1000, &req))
    {
    case READ_TIMEOUT:
        printf("[%d]   -> timeout (closed)\n", (int)me); /* 응답 없이 닫는다 (nginx 와 같음) */
        return;
    case READ_CLOSED:
    case READ_STOPPED:
        return;
    case READ_INVALID:
    {
        /* 요청 라인을 못 읽은 오류면 method 가 "" 라서 본문 있는 응답이 된다 */
        int n = build_error(out, sizeof(out), req.status, strcmp(req.method, "HEAD") == 0);
        printf("[%d]   -> %d (parse error)\n", (int)me, req.status);
        if (n > 0)
            write_all(connfd, out, (size_t)n);
        return;
    }
    case READ_OK:
        break;
    }

    /* -R 규칙에 맞는 경로는 백엔드로 전달, 아니면 정적 파일 (D2·D4). 요청 경로는 로그에만 남긴다 (D7) */
    struct route *rt = route_find(w->routes, w->nroutes, req.uri);
    int status = rt ? proxy_forward(connfd, buf, &req, rt, client_ip, w->timeout_sec)
                    : serve_static(connfd, w->root_real, &req);
    if (status < 0)
        printf("[%d]   %s %s -> (client gone)\n", (int)me, req.method, req.uri);
    else
        printf("[%d]   %s %s -> %d\n", (int)me, req.method, req.uri, status);
}

int worker_main(void *arg)
{
    struct worker_ctx *w = arg;
    char *buf = malloc(CONN_BUF_SIZE); /* worker 마다 자기 버퍼 (fork 뒤라 서로 안 겹침) */
    if (buf == NULL)
        return 1;

    while (!stop_requested())
    {
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        char host[NI_MAXHOST], serv[NI_MAXSERV];

        /* 모든 worker 가 같은 listenfd 에서 accept 하고, 커널이 손님 하나를 한 worker 에게만 준다 */
        int connfd = accept(w->listenfd, (struct sockaddr *)&addr, &addrlen);
        if (connfd < 0)
        {
            if (errno != EINTR) /* EINTR 이면 종료 시그널일 수 있으니 while 조건으로 */
                perror("accept");
            continue;
        }
        if (getnameinfo((struct sockaddr *)&addr, addrlen, host, sizeof(host), serv, sizeof(serv),
                        NI_NUMERICHOST | NI_NUMERICSERV) == 0)
            printf("[%d] connected: %s:%s\n", (int)getpid(), host, serv);
        else
            strcpy(host, "unknown"); /* X-Forwarded-For 에 쓰이므로 비워 두지 않는다 */

        handle_conn(connfd, buf, host, w);
        fflush(stdout); /* 여러 worker 의 로그가 줄 단위로 섞이도록 연결마다 비운다 */
        close(connfd);
    }
    free(buf);
    return 0;
}
