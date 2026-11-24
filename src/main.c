#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "net.h"

/* #1 단계: 손님 한 명씩 받아 보낸 바이트를 그대로 돌려준다 (CSAPP 11.4.9 echo 서버 구조).
 * HTTP 파싱(#2)·응답(#3)·동시성(#7)이 붙으면 이 부분이 바뀐다. */
static void echo(int connfd)
{
    char buf[8192];
    ssize_t n;

    /* read 가 0 을 돌려주면 손님이 연결을 닫은 것 (EOF) */
    while ((n = read(connfd, buf, sizeof(buf))) > 0) {
        if (write(connfd, buf, (size_t)n) != n)
            return;
    }
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

        echo(connfd);
        close(connfd);
    }
}
