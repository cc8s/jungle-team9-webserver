#include "http_request.h"
#include "http_response.h"
#include "http_connection.h"
#include <sys/types.h>
#include <sys/socket.h> // socket, bind, listen, accept
#include <netinet/in.h> // sockaddr_in, INADDR_ANY, htons
#include <arpa/inet.h>  // inet_pton, inet_ntop: IP 주소 변환
#include <unistd.h>     // read, write, close
#include <fcntl.h>      // open, blocking, nonblocking
#include <signal.h>
#include <stdio.h>      // printf, perror
#include <stdlib.h>     // exit, EXIT_FAILURE
#include <string.h>     // memset, strlen



// void doit(int fd);
// void read_requesthdrs(rio_t *rp);
// int parse_uri(char *uri, char *fname, char *cgiargs);
// void serve_static(char *fname, int fsize);
// void get_filetype(char *fname, char *ftype);
// void serve_dynamic(int fd, char *fname, char *cgiargs);
// void client_error(int fd, int *cause, char *errnum, char *shortmsg, char *longmsg);

int main(int argc, char **argv){
    // 0. init socket descriptor 
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0); 
    if (sock_fd == -1){
        perror("socket error");
        return EXIT_FAILURE;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }

    // 1. set socket detail: Socket 주소를 reuse 한다? 
    int on = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1){
        perror("setsockopt");
        close(sock_fd);
        return EXIT_FAILURE;
    };
    
    // 2. bind address 
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8000);
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // Set 32bit bytes ADDR : sockaddr -> realaddr 
    
    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(sock_fd);
        return EXIT_FAILURE;
    }

    // 3. listen 
    if (listen(sock_fd, 1) == -1) {
        perror("listen");
        close(sock_fd);
        return EXIT_FAILURE;
    }

    // 4. accept : Blocking, Process single request
    while (1) {
        // 기본 블로킹 모드: 연결이 없으면 여기서 기다린다.
        int conn_fd = accept(sock_fd, NULL, NULL);
        if (conn_fd == -1) {
            perror("accept");
            continue;
        }

        // accept 성공 직후부터 헤더 전체 수신 기한을 계산한다.
        int64_t accepted_at = http_now_ms();
        if (accepted_at < 0) {
            perror("clock_gettime");
            close(conn_fd);
            continue;
        }

        // 리스닝 소켓은 유지하고, 이 연결의 I/O만 논블로킹으로 처리한다.
        int flags = fcntl(conn_fd, F_GETFL);
        if (flags < 0 || fcntl(conn_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            perror("fcntl O_NONBLOCK");
            close(conn_fd);
            continue;
        }

        // 연결 안에서 요청을 순서대로 재사용 처리한 뒤 다음 accept로 이동한다.
        handle_connection(conn_fd, accepted_at);

        close(conn_fd);
    }
}
