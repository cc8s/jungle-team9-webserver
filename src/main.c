#include <sys/types.h>
#include <sys/socket.h> // socket, bind, listen, accept
#include <netinet/in.h> // sockaddr_in, INADDR_ANY, htons
#include <arpa/inet.h>  // inet_pton, inet_ntop: IP 주소 변환
#include <unistd.h>     // read, write, close
#include <fcntl.h>      // open, blocking, nonblocking
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
    // TODO:
    // 0. init socket descriptor 
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0); // sock_addr? 
    if (sock_fd == -1){
        perror("socket error");
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

    // 4. accept
    while (1) {
        // 기본 블로킹 모드: 연결이 없으면 여기서 기다린다.
        int conn_fd = accept(sock_fd, NULL, NULL);
        if (conn_fd == -1) {
            perror("accept");
            continue;
        }

        // TODO: conn_fd로 요청을 읽고 응답을 보내는 코드를 작성한다.
        // 이 처리가 끝난 뒤에만 다음 연결을 accept한다.

        close(conn_fd);
    }
}
