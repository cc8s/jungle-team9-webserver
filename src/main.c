#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "net.h"
#include "request.h"
#include "response.h"
#include "static.h"

/* 요청 하나를 담을 수 있는 크기: 헤더 상한 + 본문 상한. 1MB 넘으니 스택 대신 main 에서 한 번 malloc */
#define CONN_BUF_SIZE (REQ_MAX_HEADER + REQ_MAX_BODY)

/* 요청 하나를 다 읽어 파싱하고 응답 하나를 보낸 뒤 닫는다 (⑤ 는 항상 Connection: close, D5).
 * 지금은 모든 요청을 정적 파일로 처리한다. 리버스 프록시 경로(-R)는 #8 에서 여기서 갈라진다. */
static void handle_conn(int connfd, char *buf, const char *root_real)
{
    struct request req;
    char out[1024];
    size_t len = 0;
    enum parse_result r = PARSE_INCOMPLETE;

    /* 바이트가 올 때마다 처음부터 다시 파싱 (파서가 상태를 저장하지 않으므로) */
    while (r == PARSE_INCOMPLETE) {
        ssize_t n = read(connfd, buf + len, CONN_BUF_SIZE - len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) /* 오류이거나 손님이 요청 도중에 닫음 → 보낼 상대가 없으니 그냥 끝 */
            return;
        len += (size_t)n;
        r = parse_request(buf, len, &req);
    }

    if (r == PARSE_ERROR) {
        /* 요청 라인을 못 읽은 오류면 method 가 "" 라서 본문 있는 응답이 된다 */
        int n = build_error(out, sizeof(out), req.status, strcmp(req.method, "HEAD") == 0);
        printf("  -> %d (parse error)\n", req.status);
        if (n > 0)
            write_all(connfd, out, (size_t)n);
        return;
    }

    /* 요청 경로는 로그에만 남긴다 (응답 본문에는 넣지 않음, D7) */
    int status = serve_static(connfd, root_real, &req);
    if (status < 0)
        printf("  %s %s -> (client gone)\n", req.method, req.uri);
    else
        printf("  %s %s -> %d\n", req.method, req.uri, status);
}

int main(int argc, char **argv)
{
    struct config cfg;
    const char *err = NULL;
    int listenfd;

    if (parse_args(argc, argv, &cfg, &err) < 0) {
        fprintf(stderr, "error: %s\n", err);
        fprintf(stderr, "usage: %s -p <port> -r <static_root> [-R /prefix=host:port[,host:port]]... [-t sec]\n",
                argv[0]);
        return 1;
    }

    listenfd = open_listenfd(cfg.port);
    if (listenfd < 0) {
        fprintf(stderr, "error: cannot listen on port %s\n", cfg.port);
        return 1;
    }
    /* 루트는 시작할 때 한 번 실제 경로로 바꿔 둔다 (D6 의 "루트 안인가" 비교 기준) */
    char root_real[PATH_MAX];
    if (realpath(cfg.root, root_real) == NULL) {
        fprintf(stderr, "error: static root not found: %s\n", cfg.root);
        return 1;
    }

    char *buf = malloc(CONN_BUF_SIZE);
    if (buf == NULL) {
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }
    printf("listening on port %s (root=%s)\n", cfg.port, cfg.root);
    fflush(stdout);

    for (;;) {
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        char host[NI_MAXHOST], serv[NI_MAXSERV];

        /* 손님이 올 때까지 여기서 잠든다 (blocking) */
        int connfd = accept(listenfd, (struct sockaddr *)&addr, &addrlen);
        if (connfd < 0) {
            if (errno == EINTR) /* 시그널 때문에 깨어난 것뿐이면 다시 기다린다 */
                continue;
            perror("accept");
            continue;
        }
        if (getnameinfo((struct sockaddr *)&addr, addrlen, host, sizeof(host), serv, sizeof(serv),
                        NI_NUMERICHOST | NI_NUMERICSERV) == 0)
            printf("connected: %s:%s\n", host, serv);
        fflush(stdout);

        handle_conn(connfd, buf, root_real);
        fflush(stdout);
        close(connfd);
    }
}
