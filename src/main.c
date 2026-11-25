#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "config.h"
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
                "usage: %s -p <port> -r <static_root> [-R /prefix=host:port[,host:port]]... [-t sec] [-w workers]\n",
                argv[0]);
        return 1;
    }

    /* 루트는 시작할 때 한 번 실제 경로로 바꿔 둔다 (D6 의 "루트 안인가" 비교 기준) */
    char root_real[PATH_MAX];
    if (realpath(cfg.root, root_real) == NULL) {
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

    /* worker 수를 안 정했으면 코어 수만큼 (worker 하나가 코어 하나를 쓰는 게 기본) */
    int workers = cfg.workers;
    if (workers == 0) {
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        workers = cores < 1 ? 1 : cores > MAX_WORKERS ? MAX_WORKERS : (int)cores;
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
    printf("listening on port %s (root=%s, workers=%d, timeout=%ds)\n", cfg.port, root_real, workers,
           cfg.timeout_sec);
    for (int i = 0; i < cfg.nroutes; i++) {
        printf("route %s ->", routes[i].prefix);
        for (int k = 0; k < routes[i].nbe; k++)
            printf(" %s:%s", routes[i].be[k].host, routes[i].be[k].port);
        printf("\n");
    }

    struct worker_ctx ctx = {.listenfd = listenfd,
                             .root_real = root_real,
                             .timeout_sec = cfg.timeout_sec,
                             .routes = routes,
                             .nroutes = cfg.nroutes};
    int rc = prefork_run(workers, worker_main, &ctx);

    printf("shutting down\n");
    close(listenfd);
    return rc == 0 ? 0 : 1;
}
