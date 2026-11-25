#include "net.h"

#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define LISTENQ 1024 /* 아직 accept 하지 않은 연결을 커널이 쌓아 둘 수 있는 개수 */

int open_listenfd(const char *port)
{
    struct addrinfo hints, *list, *p;
    int listenfd = -1;
    int optval = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;             /* TCP */
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG; /* 이 기계의 모든 주소에서 받기 */
    hints.ai_flags |= AI_NUMERICSERV;            /* port 는 숫자만 (서비스 이름 해석 안 함) */
    if (getaddrinfo(NULL, port, &hints, &list) != 0)
        return -1;

    /* 후보 주소를 하나씩 시도해 bind 에 성공하는 첫 주소를 쓴다 */
    for (p = list; p != NULL; p = p->ai_next)
    {
        listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listenfd < 0)
            continue;

        /* 서버를 껐다 바로 켤 때 이전 연결의 TIME_WAIT 때문에 bind 가 실패하지 않도록 */
        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

        if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0)
            break;
        close(listenfd);
        listenfd = -1;
    }
    freeaddrinfo(list);

    if (listenfd < 0)
        return -1;
    if (listen(listenfd, LISTENQ) < 0)
    {
        close(listenfd);
        return -1;
    }
    return listenfd;
}
