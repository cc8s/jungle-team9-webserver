#include "http_request.h"
#include "http_response.h"
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
        int64_t header_deadline = accepted_at + HTTP_HEADER_TIMEOUT_MS;

        // 리스닝 소켓은 유지하고, 이 연결의 I/O만 논블로킹으로 처리한다.
        int flags = fcntl(conn_fd, F_GETFL);
        if (flags < 0 || fcntl(conn_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            perror("fcntl O_NONBLOCK");
            close(conn_fd);
            continue;
        }

        // A: 요청 수신·파싱. B에서 사용할 때까지 buffer를 유지한다.
        http_buffer buffer = {0};
        http_request request = {0};
        if (http_receive_request(conn_fd, &buffer, &request, header_deadline) == HTTP_COMPLETE) {
            printf("request: %.*s %.*s (%zu bytes)\n",
                   (int)request.method.length, buffer.data + request.method.offset,
                   (int)request.target.length, buffer.data + request.target.offset,
                   request.consumed);
        } else {
            fprintf(stderr, "request error: %d\n", request.error);
        }
        // B: 파싱 결과로 응답 생성·쓰기. 실패하면 추가 응답 없이 정리한다.
        if (http_send_response(conn_fd, &buffer, &request) < 0)
            fprintf(stderr, "response send failed\n");
        free(buffer.data);
        
        // 이 처리가 끝난 뒤에만 다음 연결을 accept한다.

        close(conn_fd);
    }
}
