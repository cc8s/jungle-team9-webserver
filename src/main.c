#include <limits.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "evloop.h"
#include "l4.h"
#include "net.h"
#include "prefork.h"
#include "route.h"
#include "signals.h"
#include "worker.h"

/* 설정을 읽고, 대기 소켓을 연 뒤, worker 들을 띄워 맡긴다 (D1 ⑤ prefork + 타임아웃).
 * 요청 처리는 worker.c, worker 관리는 prefork.c. */
int main(int argc, char **argv)
{
    struct config cfg;
    const char *err = NULL;

    if (parse_args(argc, argv, &cfg, &err) < 0) {
        fprintf(stderr, "error: %s\n", err);
        fprintf(stderr,
                "usage: %s -p <port> -r <static_root> [-R /prefix=host:port[,host:port]]... [-t sec] [-w workers] [-m epoll|prefork]\n"
                "       %s -p <port> -L host:port[,host:port] [-t sec] [-w workers]    (L4 relay)\n",
                argv[0], argv[0]);
        return 1;
    }

    /* 루트는 시작할 때 한 번 실제 경로로 바꿔 둔다 (D6 의 "루트 안인가" 비교 기준) */
    char root_real[PATH_MAX] = "";
    if (!cfg.l4 && realpath(cfg.root, root_real) == NULL) {
        fprintf(stderr, "error: static root not found: %s\n", cfg.root);
        return 1;
    }

    /* -R 규칙을 해석해 둔다 (형식은 parse_args 가 이미 확인했지만 규칙 구조체는 여기서 만든다) */
    struct route routes[MAX_ROUTES];
    for (int i = 0; i < cfg.nroutes; i++) {
        if (route_parse(cfg.routes[i], &routes[i], &err) < 0) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
    }

    /* L4 중계(-L): 백엔드 목록 하나를 규칙 하나처럼 쓴다 (접두어는 안 씀). 중계 루프가 Round-Robin 을 재사용 */
    struct route l4route;
    if (cfg.l4) {
        memset(&l4route, 0, sizeof(l4route));
        if (backends_parse(cfg.l4, &l4route, &err) < 0) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
    }

    /* worker 수를 안 정했으면 코어 수만큼 (worker 하나가 코어 하나를 쓰는 게 기본) */
    int workers = cfg.workers;
    if (workers == 0) {
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        workers = cores < 1 ? 1 : cores > MAX_WORKERS ? MAX_WORKERS : (int)cores;
    }

    /* ⑥ 은 worker 하나가 연결을 수천 개 들고 있으니 열 수 있는 fd 상한을 허용된 최대로 올린다 (기본 1024 에서 막힘) */
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    /* ⑥ 은 요청 중에 DNS 로 막히지 않도록 백엔드 주소를 시작할 때 한 번 해석한다 (D2 이유 보강 7) */
    if (cfg.model == MODEL_EPOLL && route_resolve(routes, cfg.nroutes, &err) < 0) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    if (cfg.l4 && route_resolve(&l4route, 1, &err) < 0) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }

    /* 대기 소켓은 fork 전에 부모가 한 번 연다 → 모든 worker 가 같은 소켓을 물려받는다 */
    int listenfd = open_listenfd(cfg.port);
    if (listenfd < 0) {
        fprintf(stderr, "error: cannot listen on port %s\n", cfg.port);
        return 1;
    }
    /* fork 전에 등록 → worker 도 같은 처리(종료 표시, SIGPIPE 무시)를 물려받는다 */
    if (install_signals() < 0) {
        perror("sigaction");
        return 1;
    }
    if (cfg.l4) {
        printf("listening on port %s (L4 relay, workers=%d, timeout=%ds) ->", cfg.port, workers, cfg.timeout_sec);
        for (int k = 0; k < l4route.nbe; k++)
            printf(" %s:%s", l4route.be[k].host, l4route.be[k].port);
        printf("\n");
    } else {
        printf("listening on port %s (root=%s, model=%s, workers=%d, timeout=%ds)\n", cfg.port, root_real,
               cfg.model == MODEL_EPOLL ? "epoll" : "prefork", workers, cfg.timeout_sec);
    }
    for (int i = 0; i < cfg.nroutes; i++) {
        printf("route %s ->", routes[i].prefix);
        for (int k = 0; k < routes[i].nbe; k++)
            printf(" %s:%s", routes[i].be[k].host, routes[i].be[k].port);
        printf("\n");
    }

    struct worker_ctx ctx = {.listenfd = listenfd,
                             .root_real = root_real,
                             .timeout_sec = cfg.timeout_sec,
                             .routes = cfg.l4 ? &l4route : routes,
                             .nroutes = cfg.l4 ? 1 : cfg.nroutes};
    work_fn work = cfg.l4 ? l4_main : cfg.model == MODEL_EPOLL ? evloop_main : worker_main;
    int rc = prefork_run(workers, work, &ctx);

    printf("shutting down\n");
    close(listenfd);
    return rc == 0 ? 0 : 1;
}
