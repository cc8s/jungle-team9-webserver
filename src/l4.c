#include "l4.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "signals.h"

#define MAX_EVENTS 256
#define BUF_CAP (16 * 1024)
#define SCAN_MS 100

enum ev_kind { EV_LISTEN, EV_CLIENT, EV_BACKEND };

struct l4conn;

struct ev
{
    enum ev_kind kind;
    struct l4conn *c;
};

/* 한쪽 방향 (손님→백엔드 또는 백엔드→손님) 의 바이트 통로 */
struct pipe_dir
{
    char *buf; /* 처음 쓸 때 할당 */
    size_t len, off;
    int eof; /* 읽는 쪽이 닫음 (더 안 들어옴) */
    int shut; /* 쓰는 쪽에 "더 안 보낸다" 를 알렸음 (shutdown SHUT_WR) */
};

struct l4conn
{
    struct ev cev, bev;
    int fd, be_fd;
    uint32_t cmask, bmask;
    int connecting;
    long deadline;
    struct route *rt;
    int be_start, be_k;
    struct pipe_dir c2b, b2c;
    struct l4conn *prev, *next, *dead_next;
    int dead;
};

struct loop
{
    int ep;
    const struct worker_ctx *w;
    long timeout_ms, now, drain_deadline;
    struct l4conn *head, *dead;
    int nconn, draining;
    struct ev listen_ev;
};

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void arm(struct loop *lp, struct l4conn *c)
{
    c->deadline = lp->now + lp->timeout_ms;
}

static void kill_conn(struct loop *lp, struct l4conn *c)
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
    if (c->be_fd >= 0)
        close(c->be_fd);
    close(c->fd);
    free(c->c2b.buf);
    free(c->b2c.buf);
    c->c2b.buf = c->b2c.buf = NULL;
    lp->nconn--;
    c->dead_next = lp->dead;
    lp->dead = c;
}

static void reap(struct loop *lp)
{
    while (lp->dead) {
        struct l4conn *c = lp->dead;
        lp->dead = c->dead_next;
        free(c);
    }
}

/* 관심 있는 이벤트를 상태에서 다시 계산해 epoll 에 반영한다 */
static void update_masks(struct loop *lp, struct l4conn *c)
{
    uint32_t cm = 0, bm = 0;

    if (c->connecting)
        bm = EPOLLOUT;
    else {
        /* 읽기: 상대로 보낼 통로가 비어 있고, 읽는 쪽이 안 닫혔을 때만 (밀리면 읽지 않아 느린 쪽이 빠른 쪽을 막는다) */
        if (!c->c2b.eof && c->c2b.len == c->c2b.off)
            cm |= EPOLLIN;
        if (!c->b2c.eof && c->b2c.len == c->b2c.off)
            bm |= EPOLLIN;
        /* 쓰기: 보낼 게 남았을 때 */
        if (c->b2c.len > c->b2c.off)
            cm |= EPOLLOUT;
        if (c->c2b.len > c->c2b.off)
            bm |= EPOLLOUT;
    }
    if (cm != c->cmask) {
        struct epoll_event e = {.events = cm, .data.ptr = &c->cev};
        epoll_ctl(lp->ep, EPOLL_CTL_MOD, c->fd, &e);
        c->cmask = cm;
    }
    if (c->be_fd >= 0 && bm != c->bmask) {
        struct epoll_event e = {.events = bm, .data.ptr = &c->bev};
        epoll_ctl(lp->ep, EPOLL_CTL_MOD, c->be_fd, &e);
        c->bmask = bm;
    }
}

static void connect_next(struct loop *lp, struct l4conn *c);

/* 통로 d 를 읽는 fd 에서 읽어 채운다. 0 이면 이어서, -1 이면 연결을 닫을 것 */
static int pump_read(int from, struct pipe_dir *d)
{
    if (d->buf == NULL && (d->buf = malloc(BUF_CAP)) == NULL)
        return -1;
    ssize_t n = read(from, d->buf, BUF_CAP);
    if (n > 0) {
        d->len = (size_t)n;
        d->off = 0;
    } else if (n == 0) {
        d->eof = 1;
    } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        return -1;
    }
    return 0;
}

/* 통로 d 의 남은 바이트를 쓰는 fd 로 보낸다. 다 보내고 읽는 쪽이 닫혔으면 쓰는 쪽에 "끝" 을 알린다 (반닫기) */
static int pump_write(int to, struct pipe_dir *d, int *progressed)
{
    while (d->off < d->len) {
        ssize_t n = send(to, d->buf + d->off, d->len - d->off, MSG_NOSIGNAL);
        if (n > 0) {
            d->off += (size_t)n;
            *progressed = 1;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        } else {
            return -1;
        }
    }
    if (d->eof && !d->shut) {
        shutdown(to, SHUT_WR);
        d->shut = 1;
    }
    return 0;
}

/* 양쪽을 한 번 훑는다: 읽을 수 있으면 읽고, 보낼 게 있으면 보낸다. 다 끝났으면 닫는다 */
static void pump(struct loop *lp, struct l4conn *c, uint32_t cev, uint32_t bev)
{
    int progressed = 0;

    if ((cev & EPOLLIN) && !c->c2b.eof && c->c2b.len == c->c2b.off) {
        if (pump_read(c->fd, &c->c2b) < 0)
            goto die;
        progressed = 1;
    }
    if ((bev & (EPOLLIN | EPOLLHUP)) && !c->b2c.eof && c->b2c.len == c->b2c.off) {
        if (pump_read(c->be_fd, &c->b2c) < 0)
            goto die;
        progressed = 1;
    }
    if (pump_write(c->be_fd, &c->c2b, &progressed) < 0 || pump_write(c->fd, &c->b2c, &progressed) < 0)
        goto die;
    /* 읽은 직후 바로 보내 봤는데 다 보냈다면 다음 읽기가 가능해졌으니, 통로가 비는 한 계속 (한 이벤트에서 한 번 더) */
    if (c->c2b.len == c->c2b.off && !c->c2b.eof) {
        if (pump_read(c->fd, &c->c2b) < 0)
            goto die;
        if (pump_write(c->be_fd, &c->c2b, &progressed) < 0)
            goto die;
    }
    if (c->b2c.len == c->b2c.off && !c->b2c.eof) {
        if (pump_read(c->be_fd, &c->b2c) < 0)
            goto die;
        if (pump_write(c->fd, &c->b2c, &progressed) < 0)
            goto die;
    }
    if (progressed)
        arm(lp, c);
    /* 양쪽이 다 닫혔고 보낼 것도 없으면 끝 */
    if (c->c2b.eof && c->b2c.eof && c->c2b.len == c->c2b.off && c->b2c.len == c->b2c.off) {
        kill_conn(lp, c);
        return;
    }
    update_masks(lp, c);
    return;
die:
    kill_conn(lp, c);
}

/* 다음 백엔드에 non-blocking connect (D4: 실패하면 다음). 다 실패하면 손님 연결을 닫는다 (L4 는 오류 응답을 만들 줄 모른다) */
static void connect_next(struct loop *lp, struct l4conn *c)
{
    struct route *rt = c->rt;

    if (c->be_fd >= 0) {
        close(c->be_fd);
        c->be_fd = -1;
        c->bmask = 0;
    }
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
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
        struct epoll_event e = {.events = EPOLLOUT, .data.ptr = &c->bev};
        if (epoll_ctl(lp->ep, EPOLL_CTL_ADD, fd, &e) < 0) {
            close(fd);
            continue;
        }
        c->be_fd = fd;
        c->bmask = EPOLLOUT;
        c->connecting = 1;
        arm(lp, c);
        return;
    }
    kill_conn(lp, c);
}

static void on_backend_connecting(struct loop *lp, struct l4conn *c)
{
    int err = 0;
    socklen_t l = sizeof(err);
    struct sockaddr_storage peer;
    socklen_t pl = sizeof(peer);

    if (getsockopt(c->be_fd, SOL_SOCKET, SO_ERROR, &err, &l) < 0 || err != 0) {
        connect_next(lp, c);
        return;
    }
    if (getpeername(c->be_fd, (struct sockaddr *)&peer, &pl) < 0)
        return; /* 아직 연결 중 */
    c->connecting = 0;
    /* 연결되면 손님 쪽 읽기를 연다. 손님이 연결 직후 이미 보낸 바이트가 있을 수 있어 바로 한 번 훑는다 */
    c->cmask = 0;
    struct epoll_event ce = {.events = EPOLLIN, .data.ptr = &c->cev};
    epoll_ctl(lp->ep, EPOLL_CTL_MOD, c->fd, &ce);
    c->cmask = EPOLLIN;
    arm(lp, c);
    pump(lp, c, EPOLLIN, 0);
}

static void on_listen(struct loop *lp)
{
    for (int i = 0; i < 128; i++) {
        int fd = accept4(lp->w->listenfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                perror("accept4");
            return;
        }
        struct l4conn *c = calloc(1, sizeof(*c));
        if (c == NULL) {
            close(fd);
            return;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
        c->fd = fd;
        c->be_fd = -1;
        c->cev = (struct ev){EV_CLIENT, c};
        c->bev = (struct ev){EV_BACKEND, c};
        c->rt = &lp->w->routes[0];
        c->be_start = route_next_start(c->rt);
        /* 연결이 맺어질 때까지 손님 쪽은 읽지 않는다 (관심 0: 끊김만 감시) */
        struct epoll_event e = {.events = 0, .data.ptr = &c->cev};
        if (epoll_ctl(lp->ep, EPOLL_CTL_ADD, fd, &e) < 0) {
            close(fd);
            free(c);
            continue;
        }
        c->next = lp->head;
        if (lp->head)
            lp->head->prev = c;
        lp->head = c;
        lp->nconn++;
        arm(lp, c);
        connect_next(lp, c);
    }
}

static void begin_drain(struct loop *lp)
{
    lp->draining = 1;
    lp->drain_deadline = lp->now + lp->timeout_ms;
    epoll_ctl(lp->ep, EPOLL_CTL_DEL, lp->w->listenfd, NULL);
}

int l4_main(void *arg)
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
    fcntl(w->listenfd, F_SETFL, fcntl(w->listenfd, F_GETFL) | O_NONBLOCK);
    lp.listen_ev = (struct ev){EV_LISTEN, NULL};
    struct epoll_event le = {.events = EPOLLIN | EPOLLEXCLUSIVE, .data.ptr = &lp.listen_ev};
    if (epoll_ctl(lp.ep, EPOLL_CTL_ADD, w->listenfd, &le) < 0) {
        perror("epoll_ctl");
        return 1;
    }
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigprocmask(SIG_BLOCK, &block, &orig); /* evloop.c 와 같은 이유: 대기 중에만 종료 시그널을 받는다 */

    long next_scan = lp.now + SCAN_MS;
    for (;;) {
        if (stop_requested() && !lp.draining)
            begin_drain(&lp);
        if (lp.draining && (lp.nconn == 0 || lp.now >= lp.drain_deadline))
            break;
        int n = epoll_pwait(lp.ep, evs, MAX_EVENTS, (lp.nconn > 0 || lp.draining) ? SCAN_MS : 1000, &orig);
        lp.now = now_ms();
        for (int i = 0; i < n; i++) {
            struct ev *e = evs[i].data.ptr;
            uint32_t ev = evs[i].events;
            if (e->kind == EV_LISTEN) {
                if (!lp.draining)
                    on_listen(&lp);
                continue;
            }
            struct l4conn *c = e->c;
            if (c->dead)
                continue;
            if (e->kind == EV_BACKEND) {
                if (c->be_fd < 0)
                    continue;
                if (c->connecting)
                    on_backend_connecting(&lp, c);
                else if (ev & EPOLLERR)
                    kill_conn(&lp, c);
                else
                    pump(&lp, c, 0, ev);
            } else {
                if (ev & EPOLLERR)
                    kill_conn(&lp, c);
                else if (!c->connecting)
                    pump(&lp, c, ev, 0);
                else if (ev & EPOLLHUP)
                    kill_conn(&lp, c); /* 연결이 되기도 전에 손님이 끊음 */
            }
        }
        if (lp.now >= next_scan) {
            for (struct l4conn *c = lp.head, *nx; c; c = nx) {
                nx = c->next;
                if (!c->dead && lp.now >= c->deadline) {
                    if (c->connecting)
                        connect_next(&lp, c); /* 연결이 안 끝남 → 다음 백엔드 */
                    else
                        kill_conn(&lp, c); /* 양쪽 모두 -t 동안 조용함: 멈춘 백엔드·쉬는 손님 */
                    if (!c->dead)
                        arm(&lp, c);
                }
            }
            next_scan = lp.now + SCAN_MS;
        }
        reap(&lp);
    }
    for (struct l4conn *c = lp.head, *nx; c; c = nx) {
        nx = c->next;
        kill_conn(&lp, c);
    }
    reap(&lp);
    close(lp.ep);
    return 0;
}
