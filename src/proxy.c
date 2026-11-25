#include "proxy.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "path.h"
#include "response.h"
#include "signals.h"

/* 헤더 줄 [p, p+n) 이 이름 name 으로 시작하는가 ("Connection:" 처럼) */
static int is_header(const char *p, size_t n, const char *name)
{
    size_t l = strlen(name);
    return n > l && strncasecmp(p, name, l) == 0 && p[l] == ':';
}

int proxy_build_request(const char *hdr, size_t hdr_len, const char *client_ip, char *out, size_t cap)
{
    size_t o = 0;
    const char *p = hdr, *end = hdr + hdr_len;
    int first = 1;

    while (p < end) {
        const char *e = memmem(p, (size_t)(end - p), "\r\n", 2);
        size_t n = (size_t)(e - p);

        if (n == 0) { /* 빈 줄 = 헤더 끝: 우리가 넣을 헤더를 여기서 */
            int w = snprintf(out + o, cap - o, "Connection: close\r\nX-Forwarded-For: %s\r\n\r\n", client_ip);
            if (w < 0 || (size_t)w >= cap - o)
                return -1;
            return (int)(o + (size_t)w);
        }
        if (first || !(is_header(p, n, "Connection") || is_header(p, n, "Keep-Alive") ||
                       is_header(p, n, "Proxy-Connection") || is_header(p, n, "X-Forwarded-For"))) {
            if (o + n + 2 > cap)
                return -1;
            memcpy(out + o, p, n + 2);
            o += n + 2;
        }
        first = 0;
        p = e + 2;
    }
    return -1; /* 빈 줄이 없음: 파서가 통과시킨 요청에는 없는 경우 */
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int ms_left(long deadline_ms)
{
    long l = deadline_ms - now_ms();
    return l < 0 ? 0 : (int)l;
}

/* 백엔드에 연결한다. 연결이 timeout_ms 안에 안 끝나면 포기 (막힌 백엔드가 worker 를 붙잡지 않게).
 * 성공이면 블로킹 모드의 fd, 실패면 -1 */
static int connect_backend(const struct backend *b, int timeout_ms)
{
    struct addrinfo hints, *list, *a;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    if (getaddrinfo(b->host, b->port, &hints, &list) != 0)
        return -1;

    long deadline = now_ms() + timeout_ms;
    for (a = list; a != NULL; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_NONBLOCK, a->ai_protocol);
        if (fd < 0)
            continue;
        int rc = connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            /* 연결이 끝나면 "쓰기 가능" 이 된다. 성공인지 실패인지는 SO_ERROR 로 */
            struct pollfd p = {.fd = fd, .events = POLLOUT};
            int err = 0;
            socklen_t el = sizeof(err);
            if (poll(&p, 1, ms_left(deadline)) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 &&
                err == 0)
                rc = 0;
        }
        if (rc == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    if (fd >= 0)
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK); /* 이후는 poll 로 기다리며 읽으니 블로킹으로 */
    return fd;
}

/* 손님에게 오류 응답. 상태 코드를 돌려주고, 쓰다가 끊기면 -1 */
static int send_error(int fd, int status, int head_only)
{
    char out[1024];
    int n = build_error(out, sizeof(out), status, head_only);
    if (n < 0 || write_all(fd, out, (size_t)n) < 0)
        return -1;
    return status;
}

/* "HTTP/1.1 200 OK" 맨 앞 줄에서 코드를 읽는다. 못 읽으면 0 */
static int status_of(const char *chunk, size_t n)
{
    if (n >= 12 && memcmp(chunk, "HTTP/1.", 7) == 0 && chunk[8] == ' ')
        return atoi(chunk + 9);
    return 0;
}

int proxy_forward(int clientfd, const char *buf, const struct request *req, struct route *rt, const char *client_ip,
                  int timeout_sec)
{
    int head_only = strcmp(req->method, "HEAD") == 0;
    char path[PATH_MAX], out[REQ_MAX_HEADER + 256];
    int timeout_ms = timeout_sec * 1000;

    /* D6: 프록시 경로도 ".." 검사까지는 한다 (파일이 아니라 realpath 는 못 쓰니 이 검사가 유일한 방어) */
    if (url_decode_path(req->uri, path, sizeof(path)) < 0)
        return send_error(clientfd, 400, head_only);
    if (has_dotdot_segment(path))
        return send_error(clientfd, 403, head_only);

    int hlen = proxy_build_request(buf, req->header_len, client_ip, out, sizeof(out));
    if (hlen < 0)
        return send_error(clientfd, 400, head_only); /* 헤더가 늘어나 상한을 넘음 */

    /* Round-Robin 으로 시작할 백엔드를 고르고, 연결이 안 되면 다음 백엔드 (D4). 모두 실패면 502 */
    int start = route_next_start(rt), be = -1;
    for (int k = 0; k < rt->nbe && be < 0; k++) {
        int fd = connect_backend(&rt->be[(start + k) % rt->nbe], timeout_ms);
        if (fd >= 0)
            be = fd;
    }
    if (be < 0)
        return send_error(clientfd, 502, head_only);

    struct timeval tv = {.tv_sec = timeout_sec, .tv_usec = 0};
    setsockopt(be, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)); /* 안 받아 가는 백엔드가 worker 를 붙잡지 않게 */

    /* 요청 보내기: 고쳐 쓴 헤더 + 본문(그대로) */
    if (write_all(be, out, (size_t)hlen) < 0 || write_all(be, req->body, req->content_length) < 0) {
        close(be);
        return send_error(clientfd, 502, head_only);
    }

    /* 응답 중계: 백엔드가 닫을 때까지 읽어 손님에게 그대로. 바이트 사이에 timeout 만큼 조용하면 포기 */
    char chunk[16 * 1024];
    int status = 0;
    int got_any = 0;
    for (;;) {
        struct pollfd p = {.fd = be, .events = POLLIN};
        int r = poll(&p, 1, timeout_ms);
        if (r < 0 && errno == EINTR) {
            /* 아직 아무것도 못 받았는데 종료 시그널이면 이 요청은 버린다 (안 그러면 느린 백엔드가 종료를 막음, #10).
             * 이미 중계를 시작했으면 끝까지 */
            if (!got_any && stop_requested()) {
                close(be);
                return -1;
            }
            continue;
        }
        if (r <= 0) { /* 시간 초과 (또는 오류) */
            close(be);
            if (!got_any)
                return send_error(clientfd, 504, head_only); /* 응답이 하나도 없음 → 504 */
            return -1; /* 이미 보낸 응답은 못 바꿈: 연결을 끊어 손님이 불완전함을 알게 */
        }
        ssize_t n = read(be, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break; /* 0 = 백엔드가 닫음 = 응답 끝 */
        if (!got_any) {
            status = status_of(chunk, (size_t)n);
            got_any = 1;
        }
        if (write_all(clientfd, chunk, (size_t)n) < 0) { /* 손님이 끊음 */
            close(be);
            return -1;
        }
    }
    close(be);
    if (!got_any)
        return send_error(clientfd, 502, head_only); /* 아무것도 안 보내고 닫음 */
    return status > 0 ? status : 502;
}
