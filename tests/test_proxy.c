#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "net.h"
#include "proxy.h"

/* ---- 요청 다시 쓰기 (순수 함수) ---- */
static char out[REQ_MAX_HEADER + 256];

#define REWRITE(hdr) proxy_build_request((hdr), strlen(hdr), "10.0.0.7", out, sizeof(out))

static void test_rewrite(void)
{
    int n = REWRITE("GET /api/x?a=1 HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\nKeep-Alive: timeout=5\r\n"
                    "X-Forwarded-For: 6.6.6.6\r\nAccept: */*\r\n\r\n");
    CHECK(n > 0);
    out[n] = '\0';
    CHECK(strncmp(out, "GET /api/x?a=1 HTTP/1.1\r\n", 25) == 0); /* 요청 라인·접두어 그대로 */
    CHECK(strstr(out, "\r\nHost: a\r\n") != NULL);
    CHECK(strstr(out, "\r\nAccept: */*\r\n") != NULL);
    CHECK(strstr(out, "keep-alive") == NULL && strstr(out, "Keep-Alive") == NULL); /* 손님의 연결 옵션은 버림 */
    CHECK(strstr(out, "6.6.6.6") == NULL);                                         /* 위조한 X-Forwarded-For 버림 */
    CHECK(strstr(out, "\r\nConnection: close\r\n") != NULL);
    CHECK(strstr(out, "\r\nX-Forwarded-For: 10.0.0.7\r\n\r\n") != NULL); /* 우리가 본 주소, 헤더 끝 */

    /* 대소문자 다른 이름도 같은 헤더로 */
    n = REWRITE("GET / HTTP/1.1\r\nHost: a\r\nCONNECTION: upgrade\r\nx-forwarded-for: 1.1.1.1\r\n\r\n");
    out[n] = '\0';
    CHECK(strstr(out, "upgrade") == NULL && strstr(out, "1.1.1.1") == NULL);

    /* 이름이 비슷한 다른 헤더는 건드리지 않음 */
    n = REWRITE("GET / HTTP/1.1\r\nHost: a\r\nConnection-Id: 7\r\n\r\n");
    out[n] = '\0';
    CHECK(strstr(out, "Connection-Id: 7\r\n") != NULL);

    /* 자리가 모자라면 -1 */
    CHECK(proxy_build_request("GET / HTTP/1.1\r\nHost: a\r\n\r\n", 28, "1.2.3.4", out, 20) == -1);
}

/* ---- 가짜 백엔드 (자식 프로세스) ----
   mode 'e': 요청을 다 읽고 "id=<id>\n" + 받은 요청 전체를 본문으로 돌려줌 (conns 개 연결을 처리하고 끝)
   mode 's': 요청을 읽고 아무 말 없이 버팀 (응답 없음 → 504)
   mode 'c': 응답 도중 연결을 끊음 (길이는 100 이라 해 놓고 3바이트만) */
static int bound_port(int fd)
{
    struct sockaddr_in a;
    socklen_t l = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &l);
    return ntohs(a.sin_port);
}

static pid_t start_backend(char mode, int id, int conns, char *port_out)
{
    int lfd = open_listenfd("0");
    snprintf(port_out, 8, "%d", bound_port(lfd));
    pid_t pid = fork();
    if (pid != 0)
    {
        close(lfd);
        return pid;
    }
    for (int c = 0; c < conns; c++)
    {
        int fd = accept(lfd, NULL, NULL);
        char req[8192];
        size_t len = 0;
        while (len < sizeof(req) - 1)
        { /* 헤더 끝까지, 그리고 Content-Length 만큼 본문 */
            ssize_t n = read(fd, req + len, sizeof(req) - 1 - len);
            if (n <= 0)
                break;
            len += (size_t)n;
            req[len] = '\0';
            char *h = strstr(req, "\r\n\r\n");
            if (h)
            {
                char *cl = strstr(req, "Content-Length: ");
                size_t need = (size_t)(h + 4 - req) + (cl ? (size_t)atoi(cl + 16) : 0);
                if (len >= need)
                    break;
            }
        }
        if (mode == 's')
        {
            sleep(30); /* 응답 없이 버팀 (부모가 kill) */
        }
        else if (mode == 'c')
        {
            const char *part = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nabc";
            if (write(fd, part, strlen(part)) < 0)
                _exit(1);
        }
        else
        {
            char body[9000], head[128];
            int bl = snprintf(body, sizeof(body), "id=%d\n%s", id, req);
            int hl = snprintf(head, sizeof(head),
                              "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", bl);
            if (write(fd, head, (size_t)hl) < 0 || write(fd, body, (size_t)bl) < 0)
                _exit(1);
        }
        close(fd);
    }
    _exit(0);
}

/* proxy_forward 를 부르고 손님이 받은 응답 전체를 resp 에 */
static char resp[16384];

static int forward(const char *raw_request, struct route *rt, int timeout_sec, long *ms)
{
    struct request req;
    static char buf[REQ_MAX_HEADER + 4096];
    size_t len = strlen(raw_request);
    memcpy(buf, raw_request, len);
    if (parse_request(buf, len, &req) != PARSE_OK)
    {
        printf("  (테스트의 요청이 파서를 통과 못함: %s)\n", raw_request);
        return -99;
    }
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int st = proxy_forward(sv[0], buf, &req, rt, "10.0.0.7", timeout_sec);
    clock_gettime(CLOCK_MONOTONIC, &b);
    *ms = (b.tv_sec - a.tv_sec) * 1000L + (b.tv_nsec - a.tv_nsec) / 1000000L;
    close(sv[0]);
    size_t got = 0;
    ssize_t k;
    while ((k = read(sv[1], resp + got, sizeof(resp) - 1 - got)) > 0)
        got += (size_t)k;
    resp[got] = '\0';
    close(sv[1]);
    return st;
}

static void stop(pid_t p)
{
    kill(p, SIGKILL);
    waitpid(p, NULL, 0);
}

#define GET_API "GET /api/x HTTP/1.1\r\nHost: a\r\n\r\n"

int main(void)
{
    const char *err;
    long ms;
    char port[8], port2[8], spec[128];
    struct route rt;

    alarm(60);
    signal(SIGPIPE, SIG_IGN);
    test_rewrite();

    /* 1. 정상: GET 이 백엔드까지 가고 응답이 그대로 옴. 접두어·고친 헤더 확인 */
    pid_t b1 = start_backend('e', 1, 2, port);
    snprintf(spec, sizeof(spec), "/api=127.0.0.1:%s", port);
    CHECK(route_parse(spec, &rt, &err) == 0);
    CHECK(forward("GET /api/users?id=3 HTTP/1.1\r\nHost: a\r\nConnection: keep-alive\r\n\r\n", &rt, 3, &ms) == 200);
    CHECK(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(strstr(resp, "GET /api/users?id=3 HTTP/1.1\r\n") != NULL); /* 접두어를 붙인 채 */
    CHECK(strstr(resp, "\r\nConnection: close\r\n") != NULL);
    CHECK(strstr(resp, "keep-alive") == NULL);
    CHECK(strstr(resp, "X-Forwarded-For: 10.0.0.7\r\n") != NULL);

    /* 2. POST 본문이 그대로 감 */
    CHECK(forward("POST /api/x HTTP/1.1\r\nHost: a\r\nContent-Length: 11\r\n\r\nhello world", &rt, 3, &ms) == 200);
    CHECK(strstr(resp, "POST /api/x HTTP/1.1") != NULL);
    CHECK(strstr(resp, "\r\n\r\nhello world") != NULL);
    stop(b1);

    /* 3. 백엔드가 꺼져 있음 → 502 (방금 닫은 포트: 연결 거절) */
    CHECK(forward(GET_API, &rt, 3, &ms) == 502);
    CHECK(strncmp(resp, "HTTP/1.1 502 Bad Gateway\r\n", 26) == 0);
    CHECK(strstr(resp, "127.0.0.1") == NULL); /* 백엔드 주소를 손님에게 알리지 않음 (D7) */

    /* 4. 응답을 안 하는 백엔드 → 타임아웃 후 504, 시간은 timeout 근처 */
    pid_t bs = start_backend('s', 0, 1, port);
    snprintf(spec, sizeof(spec), "/api=127.0.0.1:%s", port);
    CHECK(route_parse(spec, &rt, &err) == 0);
    CHECK(forward(GET_API, &rt, 1, &ms) == 504);
    CHECK(strncmp(resp, "HTTP/1.1 504 Gateway Timeout\r\n", 30) == 0);
    CHECK(ms >= 900 && ms < 3000);
    stop(bs);

    /* 5. 응답 도중 백엔드가 끊음 → 서버 생존, 받은 만큼은 중계 */
    pid_t bc = start_backend('c', 0, 1, port);
    snprintf(spec, sizeof(spec), "/api=127.0.0.1:%s", port);
    CHECK(route_parse(spec, &rt, &err) == 0);
    CHECK(forward(GET_API, &rt, 3, &ms) == 200);
    CHECK(strstr(resp, "abc") != NULL);
    stop(bc);

    /* 6. Round-Robin: 백엔드 둘, 요청 4개 → 각각 2번 */
    pid_t r1 = start_backend('e', 1, 2, port), r2 = start_backend('e', 2, 2, port2);
    snprintf(spec, sizeof(spec), "/api=127.0.0.1:%s,127.0.0.1:%s", port, port2);
    CHECK(route_parse(spec, &rt, &err) == 0);
    int hits[3] = {0, 0, 0};
    for (int i = 0; i < 4; i++)
    {
        CHECK(forward(GET_API, &rt, 3, &ms) == 200);
        char *p = strstr(resp, "id=");
        if (p && (p[3] == '1' || p[3] == '2'))
            hits[p[3] - '0']++;
    }
    CHECK(hits[1] == 2 && hits[2] == 2);
    stop(r1);
    stop(r2);

    /* 7. 장애 우회: 첫 백엔드(죽은 포트)가 앞에 있어도 살아 있는 쪽으로 간다. 둘 다 죽으면 502 */
    pid_t r3 = start_backend('e', 3, 3, port2);
    snprintf(spec, sizeof(spec), "/api=127.0.0.1:%s,127.0.0.1:%s", port, port2); /* port 는 방금 죽인 r1 */
    CHECK(route_parse(spec, &rt, &err) == 0);
    for (int i = 0; i < 3; i++)
    {
        CHECK(forward(GET_API, &rt, 3, &ms) == 200); /* 시작 백엔드가 어느 쪽이든 */
        CHECK(strstr(resp, "id=3") != NULL);
    }
    stop(r3);
    CHECK(forward(GET_API, &rt, 3, &ms) == 502);

    /* 8. D6: 프록시 경로의 .. 와 NUL */
    CHECK(forward("GET /api/../admin HTTP/1.1\r\nHost: a\r\n\r\n", &rt, 3, &ms) == 403);
    CHECK(forward("GET /api/%2e%2e/admin HTTP/1.1\r\nHost: a\r\n\r\n", &rt, 3, &ms) == 403);
    CHECK(forward("GET /api/a%00b HTTP/1.1\r\nHost: a\r\n\r\n", &rt, 3, &ms) == 400);
    CHECK(forward("GET /api/a..b HTTP/1.1\r\nHost: a\r\n\r\n", &rt, 3, &ms) == 502); /* 통과, 백엔드가 없어 502 */

    CHECK_DONE();
}
