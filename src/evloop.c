#include "evloop.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "path.h"
#include "proxy.h"
#include "response.h"
#include "signals.h"
#include "static.h"

#define MAX_EVENTS 256
#define KEEPALIVE_MAX 100                      /* 연결 하나가 처리하는 요청 수 상한 (D5). 100번째 응답은 close */
#define MAX_IN (REQ_MAX_HEADER + REQ_MAX_BODY) /* 요청 하나가 차지할 수 있는 최대 */
#define OUT_CAP (16 * 1024)                    /* 프록시 중계 조각 크기 */
#define PX_HDR_CAP (REQ_MAX_HEADER + 256)
#define SCAN_MS 100 /* 마감이 지난 연결을 찾는 주기 */

enum ev_kind { EV_LISTEN, EV_CLIENT, EV_BACKEND };
enum state { S_READ, S_WRITE, S_PX_CONNECT, S_PX_SEND, S_PX_RECV };

struct conn;

/* epoll 이벤트가 어느 쪽 fd 의 것인지 알려 주는 꼬리표 (epoll_event.data.ptr 로 넘김) */
struct ev
{
    enum ev_kind kind;
    struct conn *c;
};

struct conn
{
    struct ev cev, bev; /* 손님 쪽·백엔드 쪽 꼬리표 */
    int fd, be_fd;      /* 손님 소켓, 백엔드 소켓 (없으면 -1) */
    uint32_t cmask, bmask;
    enum state st;
    long deadline; /* 이 시각까지 아무 진전이 없으면 정리 */
    struct conn *prev, *next; /* 모든 연결 목록 */
    struct conn *dead_next;   /* 이번 epoll 배치가 끝나면 free 할 연결 */
    int dead;
    char client_ip[INET6_ADDRSTRLEN];

    /* 받은 바이트. 아무것도 안 온 연결은 NULL 이라 쉬는 연결은 구조체 하나뿐 (C10K) */
    char *in;
    size_t in_len, in_cap;
    struct request req;
    int head_only, keep_alive, served;

    /* 보낼 바이트: 헤더·오류 페이지·프록시 중계 조각 */
    char *out;
    size_t out_cap, out_len, out_off;
    int file_fd; /* 정적 본문. sendfile 로 커널이 바로 소켓에 복사 */
    off_t file_off, file_left;

    /* 프록시 */
    struct route *rt;
    int be_start, be_k, got_any;
    char *px_hdr; /* 백엔드로 보낼 다시 쓴 요청 헤더 */
    size_t px_len, px_off;
};

struct loop
{
    int ep;
    const struct worker_ctx *w;
    long timeout_ms, now, drain_deadline;
    struct conn *head;
    int nconn, draining;
    struct conn *dead;
    struct ev listen_ev;
};

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* 이 연결의 마감을 지금부터 -t 초 뒤로 (진전이 있을 때마다) */
static void arm(struct loop *lp, struct conn *c)
{
    c->deadline = lp->now + lp->timeout_ms;
}

static void set_cmask(struct loop *lp, struct conn *c, uint32_t m)
{
    if (c->cmask == m)
        return;
    struct epoll_event e = {.events = m, .data.ptr = &c->cev};
    epoll_ctl(lp->ep, EPOLL_CTL_MOD, c->fd, &e); /* 관심이 0 이어도 끊김(HUP·ERR)은 알려 준다 */
    c->cmask = m;
}

static void set_bmask(struct loop *lp, struct conn *c, uint32_t m)
{
    if (c->bmask == m || c->be_fd < 0)
        return;
    struct epoll_event e = {.events = m, .data.ptr = &c->bev};
    epoll_ctl(lp->ep, EPOLL_CTL_MOD, c->be_fd, &e);
    c->bmask = m;
}

static void be_close(struct conn *c)
{
    if (c->be_fd >= 0) {
        close(c->be_fd); /* 닫으면 epoll 에서도 빠진다 */
        c->be_fd = -1;
        c->bmask = 0;
    }
}

/* 연결을 정리한다. 같은 epoll 배치에 이 연결의 이벤트가 더 남아 있을 수 있어서 free 는 배치가 끝난 뒤(reap) */
static void conn_kill(struct loop *lp, struct conn *c)
{
    if (c->dead)
        return;
    c->dead = 1;
    if (c->prev)
        c->prev->next = c->next;
    else
        lp->head = c->next;
    if (c->next)
        c->next->prev = c->prev;
    be_close(c);
    if (c->file_fd >= 0)
        close(c->file_fd);
    close(c->fd);
    free(c->in);
    free(c->out);
    free(c->px_hdr);
    c->in = c->out = c->px_hdr = NULL;
    lp->nconn--;
    c->dead_next = lp->dead;
    lp->dead = c;
}

static void reap(struct loop *lp)
{
    while (lp->dead) {
        struct conn *c = lp->dead;
        lp->dead = c->dead_next;
        free(c);
    }
}

static int ensure_out(struct conn *c, size_t cap)
{
    if (c->out_cap >= cap)
        return 0;
    char *p = realloc(c->out, cap);
    if (p == NULL)
        return -1;
    c->out = p;
    c->out_cap = cap;
    return 0;
}

static void try_write(struct loop *lp, struct conn *c);
static void connect_next(struct loop *lp, struct conn *c);

/* 이 요청의 응답이 끝났다: 연결을 유지하면 다음 요청을 기다리고, 아니면 닫는다 */
static void finish_request(struct loop *lp, struct conn *c)
{
    if (c->file_fd >= 0) {
        close(c->file_fd);
        c->file_fd = -1;
    }
    be_close(c);
    free(c->px_hdr);
    c->px_hdr = NULL;
    if (!c->keep_alive || lp->draining) {
        conn_kill(lp, c);
        return;
    }
    c->served++;
    /* 이 요청만큼 버리고, 뒤에 붙어 온 다음 요청(파이프라이닝)은 남긴다 */
    memmove(c->in, c->in + c->req.total_len, c->in_len - c->req.total_len);
    c->in_len -= c->req.total_len;
    if (c->in_len == 0) { /* 쉬는 keep-alive 연결이 메모리를 잡지 않게 */
        free(c->in);
        c->in = NULL;
        c->in_cap = 0;
    }
    free(c->out);
    c->out = NULL;
    c->out_cap = c->out_len = c->out_off = 0;
    c->st = S_READ;
    set_cmask(lp, c, EPOLLIN);
    arm(lp, c); /* 유휴 타임아웃 */
}

/* c->out (+ 파일) 을 손님에게 보낸다. 소켓이 꽉 차면 EPOLLOUT 을 걸고 돌아간다 (나중에 다시 불림) */
static void try_write(struct loop *lp, struct conn *c)
{
    for (;;) {
        if (c->out_off < c->out_len) {
            /* 헤더(와 오류 페이지)는 sendfile 본문이 뒤따르면 MSG_MORE 로 보낸다: 작은 조각 둘이 따로 나가면
             * Nagle 이 두 번째를 붙잡고 손님의 지연 ACK(약 40ms)를 기다려 응답마다 40ms 가 더 걸린다 */
            ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off,
                             MSG_NOSIGNAL | (c->file_left > 0 ? MSG_MORE : 0));
            if (n > 0) {
                c->out_off += (size_t)n;
                arm(lp, c);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                set_cmask(lp, c, EPOLLOUT);
                return;
            }
            conn_kill(lp, c); /* 손님이 끊음 */
            return;
        }
        if (c->file_left > 0) {
            ssize_t n = sendfile(c->fd, c->file_fd, &c->file_off, (size_t)c->file_left);
            if (n > 0) {
                c->file_left -= n;
                arm(lp, c);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                set_cmask(lp, c, EPOLLOUT);
                return;
            }
            conn_kill(lp, c); /* 파일이 중간에 줄었거나 손님이 끊음 */
            return;
        }
        break;
    }
    c->out_off = c->out_len = 0;
    if (c->st == S_PX_RECV) { /* 중계 중: 한 조각을 다 보냈으니 백엔드에서 다음 조각을 읽는다 */
        set_cmask(lp, c, 0);
        set_bmask(lp, c, EPOLLIN);
        return;
    }
    finish_request(lp, c);
}

static void start_write(struct loop *lp, struct conn *c)
{
    c->st = S_WRITE;
    arm(lp, c);
    try_write(lp, c);
}

/* 오류 응답을 보내고 연결을 닫는다 (D5: 오류 뒤에는 항상 close) */
static void send_error(struct loop *lp, struct conn *c, int status)
{
    be_close(c);
    c->keep_alive = 0;
    if (ensure_out(c, 1024) < 0) {
        conn_kill(lp, c);
        return;
    }
    int n = build_error(c->out, c->out_cap, status, c->head_only);
    if (n < 0) {
        conn_kill(lp, c);
        return;
    }
    c->out_len = (size_t)n;
    c->out_off = 0;
    c->file_left = 0;
    printf("[%d]   %s %s -> %d\n", (int)getpid(), c->req.method[0] ? c->req.method : "-",
           c->req.uri[0] ? c->req.uri : "-", status);
    start_write(lp, c);
}

/* ---- 정적 파일 ---- */
static void static_begin(struct loop *lp, struct conn *c)
{
    int fd;
    off_t size;
    const char *type;

    int code = static_open(lp->w->root_real, &c->req, &fd, &size, &type);
    if (code != 0) {
        send_error(lp, c, code);
        return;
    }
    if (ensure_out(c, 1024) < 0) {
        close(fd);
        conn_kill(lp, c);
        return;
    }
    int n = build_header(c->out, c->out_cap, 200, type, (size_t)size, c->keep_alive, NULL);
    if (n < 0) {
        close(fd);
        send_error(lp, c, 500);
        return;
    }
    c->out_len = (size_t)n;
    c->out_off = 0;
    if (c->head_only) { /* HEAD: 길이는 GET 과 같게, 본문만 없이 */
        close(fd);
        c->file_left = 0;
    } else {
        c->file_fd = fd;
        c->file_off = 0;
        c->file_left = size;
    }
    printf("[%d]   %s %s -> 200\n", (int)getpid(), c->req.method, c->req.uri);
    start_write(lp, c);
}

/* ---- 리버스 프록시 ---- */
static void proxy_begin(struct loop *lp, struct conn *c, struct route *rt)
{
    char path[PATH_MAX];

    c->keep_alive = 0; /* 중계한 응답은 끝을 백엔드의 close 로 안다 → 손님 연결도 닫는다 (단순화, 아래 설명) */
    /* D6: 프록시 경로도 ".." 검사까지는 한다 */
    if (url_decode_path(c->req.uri, path, sizeof(path)) < 0) {
        send_error(lp, c, 400);
        return;
    }
    if (has_dotdot_segment(path)) {
        send_error(lp, c, 403);
        return;
    }
    c->px_hdr = malloc(PX_HDR_CAP);
    if (c->px_hdr == NULL) {
        conn_kill(lp, c);
        return;
    }
    int n = proxy_build_request(c->in, c->req.header_len, c->client_ip, c->px_hdr, PX_HDR_CAP);
    if (n < 0) {
        send_error(lp, c, 400);
        return;
    }
    c->px_len = (size_t)n;
    c->px_off = 0;
    c->rt = rt;
    c->be_start = route_next_start(rt);
    c->be_k = 0;
    connect_next(lp, c);
}

/* 다음 백엔드에 non-blocking connect 를 건다 (D4: 실패하면 다음, 다 실패하면 502). 연결이 끝나면 EPOLLOUT 으로 알려 줌 */
static void connect_next(struct loop *lp, struct conn *c)
{
    struct route *rt = c->rt;

    while (c->be_k < rt->nbe) {
        const struct backend *b = &rt->be[(c->be_start + c->be_k) % rt->nbe];
        c->be_k++;
        int fd = socket(b->addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            continue;
        if (connect(fd, (const struct sockaddr *)&b->addr, b->addrlen) < 0 && errno != EINPROGRESS) {
            close(fd);
            continue;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int)); /* 요청 헤더와 본문이 따로 나가도 안 붙잡히게 */
        struct epoll_event e = {.events = EPOLLOUT, .data.ptr = &c->bev};
        if (epoll_ctl(lp->ep, EPOLL_CTL_ADD, fd, &e) < 0) {
            close(fd);
            continue;
        }
        c->be_fd = fd;
        c->bmask = EPOLLOUT;
        c->st = S_PX_CONNECT;
        set_cmask(lp, c, 0); /* 손님 쪽은 응답이 준비될 때까지 건드리지 않음 (끊김만 감시) */
        arm(lp, c);
        return;
    }
    send_error(lp, c, 502); /* 모든 백엔드에 연결 실패 */
}

/* 고쳐 쓴 요청 헤더 + 본문을 백엔드로 보낸다 */
static void px_send(struct loop *lp, struct conn *c)
{
    for (;;) {
        const char *src;
        size_t left;

        if (c->px_off < c->px_len) {
            src = c->px_hdr + c->px_off;
            left = c->px_len - c->px_off;
        } else if (c->px_off - c->px_len < c->req.content_length) {
            size_t o = c->px_off - c->px_len;
            src = c->req.body + o; /* 본문은 받은 버퍼 그대로 (프록시 중에는 손님 쪽을 읽지 않아 안 바뀜) */
            left = c->req.content_length - o;
        } else
            break;
        ssize_t n = write(c->be_fd, src, left);
        if (n > 0) {
            c->px_off += (size_t)n;
            arm(lp, c);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            set_bmask(lp, c, EPOLLOUT);
            return;
        }
        send_error(lp, c, 502);
        return;
    }
    c->st = S_PX_RECV;
    c->got_any = 0;
    set_bmask(lp, c, EPOLLIN);
    arm(lp, c);
}

/* 백엔드 응답을 한 조각 읽어 손님에게 그대로 보낸다. 다 보낼 때까지 백엔드는 더 안 읽는다 (느린 손님 대비) */
static void px_recv(struct loop *lp, struct conn *c)
{
    if (ensure_out(c, OUT_CAP) < 0) {
        conn_kill(lp, c);
        return;
    }
    ssize_t n = read(c->be_fd, c->out, c->out_cap);
    if (n > 0) {
        if (!c->got_any) {
            c->got_any = 1;
            int status = (n >= 12 && memcmp(c->out, "HTTP/1.", 7) == 0) ? atoi(c->out + 9) : 0;
            printf("[%d]   %s %s -> %d (proxy)\n", (int)getpid(), c->req.method, c->req.uri, status);
        }
        c->out_len = (size_t)n;
        c->out_off = 0;
        arm(lp, c);
        set_bmask(lp, c, 0);
        try_write(lp, c);
        return;
    }
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        return;
    /* 0 = 백엔드가 닫음 = 응답 끝 (읽기는 앞 조각을 다 보낸 뒤에만 하므로 보낼 게 남지 않았다) */
    be_close(c);
    if (!c->got_any)
        send_error(lp, c, 502); /* 아무 말도 없이 닫음 */
    else
        conn_kill(lp, c);
}

static void on_backend(struct loop *lp, struct conn *c)
{
    switch (c->st) {
    case S_PX_CONNECT: {
        int err = 0;
        socklen_t l = sizeof(err);
        struct sockaddr_storage peer;
        socklen_t pl = sizeof(peer);

        if (getsockopt(c->be_fd, SOL_SOCKET, SO_ERROR, &err, &l) < 0 || err != 0) {
            be_close(c); /* 연결 거절·실패 → 다음 백엔드 */
            connect_next(lp, c);
            return;
        }
        if (getpeername(c->be_fd, (struct sockaddr *)&peer, &pl) < 0)
            return; /* 아직 연결 중 (이미 닫은 이전 백엔드의 늦은 이벤트일 수 있음) */
        c->st = S_PX_SEND;
        px_send(lp, c);
        return;
    }
    case S_PX_SEND:
        px_send(lp, c);
        return;
    case S_PX_RECV:
        px_recv(lp, c);
        return;
    default:
        return;
    }
}

/* ---- 요청 읽기와 분기 ---- */
static void dispatch(struct loop *lp, struct conn *c)
{
    /* 1.1 은 기본 유지, close 면 닫음, 연결당 요청 수 상한과 종료 중이면 닫음 (D5) */
    c->keep_alive = c->req.keep_alive && c->served + 1 < KEEPALIVE_MAX && !lp->draining;
    struct route *rt = route_find(lp->w->routes, lp->w->nroutes, c->req.uri);
    if (rt)
        proxy_begin(lp, c, rt);
    else
        static_begin(lp, c);
}

/* 버퍼에 쌓인 바이트에서 요청을 하나씩 꺼내 처리한다. 응답이 바로 끝나면 이어서 다음 요청 (파이프라이닝) */
static void process_input(struct loop *lp, struct conn *c)
{
    while (!c->dead && c->st == S_READ && c->in_len > 0) {
        enum parse_result r = parse_request(c->in, c->in_len, &c->req);
        c->head_only = strcmp(c->req.method, "HEAD") == 0;
        if (r == PARSE_INCOMPLETE)
            return;
        if (r == PARSE_ERROR) {
            send_error(lp, c, c->req.status);
            return;
        }
        dispatch(lp, c);
    }
}

static void on_client(struct loop *lp, struct conn *c, uint32_t ev)
{
    if (ev & (EPOLLERR | EPOLLHUP)) { /* 손님이 끊음 */
        conn_kill(lp, c);
        return;
    }
    if (c->st == S_READ && (ev & EPOLLIN)) {
        if (c->in_len == c->in_cap) { /* 버퍼가 찼으면 두 배로 (상한까지) */
            size_t cap = c->in_cap ? c->in_cap * 2 : 4096;
            if (cap > MAX_IN)
                cap = MAX_IN;
            if (c->in_len >= cap) { /* 상한까지 찼는데 요청이 안 끝남: 파서가 이미 걸러야 하는 경우 */
                conn_kill(lp, c);
                return;
            }
            char *p = realloc(c->in, cap);
            if (p == NULL) {
                conn_kill(lp, c);
                return;
            }
            c->in = p;
            c->in_cap = cap;
        }
        ssize_t n = read(c->fd, c->in + c->in_len, c->in_cap - c->in_len);
        if (n > 0) {
            if (c->in_len == 0)
                arm(lp, c); /* 요청의 첫 바이트: 이 시각부터 -t 안에 다 와야 한다. 이후 바이트는 마감을 늘리지 않음 (찔끔 보내기 방어) */
            c->in_len += (size_t)n;
            process_input(lp, c);
        } else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            conn_kill(lp, c); /* 손님이 닫음 또는 오류 */
        }
    } else if (ev & EPOLLOUT) {
        try_write(lp, c);
        if (!c->dead && c->st == S_READ) /* 응답을 끝내고 keep-alive 로 돌아왔다: 버퍼에 다음 요청이 있을 수 있음 */
            process_input(lp, c);
    }
}

/* ---- 새 연결 ---- */
static void on_listen(struct loop *lp)
{
    for (int i = 0; i < 128; i++) { /* 한 번에 너무 오래 잡지 않도록 상한 */
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        int fd = accept4(lp->w->listenfd, (struct sockaddr *)&ss, &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                perror("accept4");
            return; /* EAGAIN: 다른 worker 가 가져갔거나 다 받음 */
        }
        struct conn *c = calloc(1, sizeof(*c));
        if (c == NULL) {
            close(fd);
            return;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int)); /* 보낸 만큼 바로 내보낸다 (keep-alive 에서 필수) */
        c->fd = fd;
        c->be_fd = c->file_fd = -1;
        c->cev = (struct ev){EV_CLIENT, c};
        c->bev = (struct ev){EV_BACKEND, c};
        c->st = S_READ;
        if (ss.ss_family == AF_INET)
            inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, c->client_ip, sizeof(c->client_ip));
        else if (ss.ss_family == AF_INET6)
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, c->client_ip, sizeof(c->client_ip));
        else
            strcpy(c->client_ip, "unknown"); /* X-Forwarded-For 에 쓰이므로 비워 두지 않는다 */
        struct epoll_event e = {.events = EPOLLIN, .data.ptr = &c->cev};
        if (epoll_ctl(lp->ep, EPOLL_CTL_ADD, fd, &e) < 0) {
            close(fd);
            free(c);
            continue;
        }
        c->cmask = EPOLLIN;
        arm(lp, c);
        c->next = lp->head;
        if (lp->head)
            lp->head->prev = c;
        lp->head = c;
        lp->nconn++;
    }
}

/* 마감이 지난 연결 정리 (D1 의 타임아웃). 상태마다 알맞게 */
static void expire(struct loop *lp, struct conn *c)
{
    switch (c->st) {
    case S_READ:
    case S_WRITE:
        conn_kill(lp, c); /* 느리거나 쉬는 손님: 응답 없이 닫음 (⑤ 와 같음) */
        break;
    case S_PX_CONNECT:
        be_close(c); /* 연결이 안 끝남 → 다음 백엔드 */
        connect_next(lp, c);
        break;
    case S_PX_SEND:
        send_error(lp, c, 502);
        break;
    case S_PX_RECV:
        if (!c->got_any)
            send_error(lp, c, 504); /* 백엔드가 아무 말도 안 함 */
        else
            conn_kill(lp, c); /* 이미 보낸 응답은 못 바꿈: 끊어서 불완전함을 알림 */
        break;
    }
}

static void begin_drain(struct loop *lp)
{
    lp->draining = 1;
    lp->drain_deadline = lp->now + lp->timeout_ms;
    epoll_ctl(lp->ep, EPOLL_CTL_DEL, lp->w->listenfd, NULL); /* 새 손님은 안 받음 (다른 worker 가 있다면 그쪽이 받음) */
    for (struct conn *c = lp->head, *nx; c; c = nx) {
        nx = c->next;
        if (c->st == S_READ && c->in_len == 0) /* 쉬는 연결은 바로 닫음. 처리 중인 것은 끝까지 */
            conn_kill(lp, c);
    }
}

int evloop_main(void *arg)
{
    const struct worker_ctx *w = arg;
    struct loop lp;
    struct epoll_event evs[MAX_EVENTS];
    sigset_t block, orig;

    memset(&lp, 0, sizeof(lp));
    lp.w = w;
    lp.timeout_ms = w->timeout_sec * 1000L;
    lp.now = now_ms();
    lp.ep = epoll_create1(EPOLL_CLOEXEC);
    if (lp.ep < 0)
        return 1;

    /* 모든 worker 가 같은 listenfd 를 본다: non-blocking 이어야 "내가 못 가져간" 경우에 accept 가 안 잠든다.
     * EPOLLEXCLUSIVE: 새 연결 하나에 worker 를 전부 깨우지 않고 하나만 (thundering herd 완화) */
    fcntl(w->listenfd, F_SETFL, fcntl(w->listenfd, F_GETFL) | O_NONBLOCK);
    lp.listen_ev = (struct ev){EV_LISTEN, NULL};
    struct epoll_event le = {.events = EPOLLIN | EPOLLEXCLUSIVE, .data.ptr = &lp.listen_ev};
    if (epoll_ctl(lp.ep, EPOLL_CTL_ADD, w->listenfd, &le) < 0) {
        perror("epoll_ctl");
        return 1;
    }

    /* 종료 시그널은 평소엔 막아 두고 epoll_pwait 가 기다리는 동안에만 푼다 → 놓치거나 잠들어 버리는 틈이 없음 */
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigprocmask(SIG_BLOCK, &block, &orig);

    long next_scan = lp.now + SCAN_MS;
    for (;;) {
        if (stop_requested() && !lp.draining)
            begin_drain(&lp);
        if (lp.draining && (lp.nconn == 0 || lp.now >= lp.drain_deadline))
            break;
        fflush(stdout); /* 기다리기 전에 로그를 비운다 (요청마다가 아니라 한가해질 때) */
        int n = epoll_pwait(lp.ep, evs, MAX_EVENTS, (lp.nconn > 0 || lp.draining) ? SCAN_MS : 1000, &orig);
        lp.now = now_ms();
        for (int i = 0; i < n; i++) {
            struct ev *e = evs[i].data.ptr;
            if (e->kind == EV_LISTEN) {
                if (!lp.draining)
                    on_listen(&lp);
            } else if (e->kind == EV_CLIENT) {
                if (!e->c->dead)
                    on_client(&lp, e->c, evs[i].events);
            } else if (!e->c->dead && e->c->be_fd >= 0) {
                on_backend(&lp, e->c);
            }
        }
        if (lp.now >= next_scan) {
            for (struct conn *c = lp.head, *nx; c; c = nx) {
                nx = c->next; /* expire 가 c 를 정리해도 안전하게 */
                if (!c->dead && lp.now >= c->deadline)
                    expire(&lp, c);
            }
            next_scan = lp.now + SCAN_MS;
        }
        reap(&lp);
    }

    for (struct conn *c = lp.head, *nx; c; c = nx) { /* 시간 안에 못 끝낸 연결 */
        nx = c->next;
        conn_kill(&lp, c);
    }
    reap(&lp);
    close(lp.ep);
    fflush(stdout);
    return 0;
}
