#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "check.h"
#include "net.h"

/* fd 가 실제로 묶인 포트 번호를 문자열로 */
static void bound_port(int fd, char *out, size_t n)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    int port = 0;

    getsockname(fd, (struct sockaddr *)&ss, &len);
    if (ss.ss_family == AF_INET)
        port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
    else if (ss.ss_family == AF_INET6)
        port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    snprintf(out, n, "%d", port);
}

int main(void)
{
    char port[16];

    /* "0" → OS 가 빈 포트를 골라 열어 준다 */
    int fd = open_listenfd("0");
    CHECK(fd >= 0);
    bound_port(fd, port, sizeof(port));
    CHECK(port[0] != '0');

    /* 이미 누가 듣고 있는 포트는 SO_REUSEADDR 이 있어도 bind 실패 → -1 */
    CHECK(open_listenfd(port) == -1);

    /* 숫자가 아닌 포트 (AI_NUMERICSERV 라서 "http" 같은 이름도 거절) */
    CHECK(open_listenfd("http") == -1);
    CHECK(open_listenfd("abc") == -1);

    /* 닫은 뒤에는 같은 포트를 다시 열 수 있다 */
    close(fd);
    fd = open_listenfd(port);
    CHECK(fd >= 0);
    close(fd);

    CHECK_DONE();
}
