#include "http_request.h"
#include <assert.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void error(const char *text, http_request_error expected)
{
    http_request r = {0};
    assert(http_parse_request((const unsigned char *)text, strlen(text), &r) == HTTP_PARSE_ERROR);
    assert(r.error == expected);
}

static void *trickle(void *argument)
{
    int fd = *(int *)argument;
    struct timespec delay = {0, 20000000};
    for (int i = 0; i < 20; i++) {
        nanosleep(&delay, NULL);
        assert(write(fd, "G", 1) == 1);
    }
    return NULL;
}

static void test_header_timeout(void)
{
    int sockets[2];
    http_buffer buffer = {0};
    http_request r = {0};
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    int64_t start = http_now_ms();
    assert(http_receive_request(sockets[1], &buffer, &r,
           start + HTTP_HEADER_TIMEOUT_MS) == HTTP_PARSE_ERROR);
    assert(r.error == HTTP_HEADER_TIMEOUT);
    int64_t elapsed = http_now_ms() - start;
    assert(elapsed >= 5000 && elapsed < 7000);
    free(buffer.data);
    close(sockets[0]); close(sockets[1]);

    /* Partial input must not reset the absolute deadline. */
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    pthread_t sender;
    assert(pthread_create(&sender, NULL, trickle, &sockets[0]) == 0);
    buffer = (http_buffer){0}; r = (http_request){0};
    start = http_now_ms();
    assert(http_receive_request(sockets[1], &buffer, &r, start + 200) == HTTP_PARSE_ERROR);
    assert(r.error == HTTP_HEADER_TIMEOUT && buffer.length > 0);
    elapsed = http_now_ms() - start;
    assert(elapsed >= 200 && elapsed < 1000);
    assert(pthread_join(sender, NULL) == 0);
    free(buffer.data);
    close(sockets[0]); close(sockets[1]);

    /* Even available input cannot extend an already expired deadline. */
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(write(sockets[0], "GET /", 5) == 5);
    buffer = (http_buffer){0}; r = (http_request){0};
    assert(http_receive_request(sockets[1], &buffer, &r, http_now_ms()-1) == HTTP_PARSE_ERROR);
    assert(r.error == HTTP_HEADER_TIMEOUT);
    free(buffer.data);
    close(sockets[0]); close(sockets[1]);
}

static void *body_trickle(void *argument)
{
    int fd = *(int *)argument;
    struct timespec delay = {2, 0};
    for (int i = 0; i < 4; i++) {
        nanosleep(&delay, NULL);
        assert(write(fd, "x", 1) == 1);
    }
    return NULL;
}

static void test_body_policy(void)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const char *get = "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\n";
    assert(write(pair[0], get, strlen(get)) == (ssize_t)strlen(get));
    http_buffer buffer = {0};
    http_request r = {0};
    int64_t start = http_now_ms();
    assert(http_receive_request(pair[1], &buffer, &r, start + 5000) == HTTP_COMPLETE);
    assert(http_now_ms()-start < 1000 && r.consumed == strlen(get) && r.content_length == 100);
    free(buffer.data); close(pair[0]); close(pair[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const char *post = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\na";
    assert(write(pair[0], post, strlen(post)) == (ssize_t)strlen(post));
    pthread_t sender;
    assert(pthread_create(&sender, NULL, body_trickle, &pair[0]) == 0);
    buffer = (http_buffer){0}; r = (http_request){0};
    start = http_now_ms();
    assert(http_receive_request(pair[1], &buffer, &r, start + 5000) == HTTP_PARSE_ERROR);
    assert(r.error == HTTP_BODY_TIMEOUT && r.headers_complete);
    int64_t elapsed = http_now_ms()-start;
    assert(elapsed >= 10000 && elapsed < 12000);
    assert(pthread_join(sender, NULL) == 0);
    free(buffer.data); close(pair[0]); close(pair[1]);
}

int main(void)
{
    const unsigned char data[] = "POST /file?q=1 HTTP/1.1\r\nhOsT: localhost\r\nContent-Length: 4\r\n\r\n\0\r\nxNEXT";
    http_request r = {0};
    size_t request_length = sizeof(data)-1-4;
    for (size_t n = 0; n < request_length; n++)
        assert(http_parse_request(data, n, &r) == HTTP_NEED_MORE);
    assert(http_parse_request(data, sizeof(data)-1, &r) == HTTP_COMPLETE);
    assert(r.content_length == 4 && r.consumed == request_length);
    assert(r.method.length == 4 && !memcmp(data + r.method.offset, "POST", 4));
    assert(r.target.length == 9 && !memcmp(data + r.target.offset, "/file?q=1", 9));
    error("GET / HTTP/1.1\r\n\r\n", HTTP_BAD_REQUEST);
    error("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n", HTTP_BAD_REQUEST);
    error("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: x\r\n\r\n", HTTP_BAD_REQUEST);
    error("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n", HTTP_BAD_REQUEST);
    error("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 999999999999999999999999\r\n\r\n", HTTP_BAD_REQUEST);
    error("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n", HTTP_BODY_TOO_LARGE);
    error("GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", HTTP_UNSUPPORTED_TRANSFER);
    error("GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n", HTTP_BAD_REQUEST);
    error("GET / HTTP/1.0\r\nHost: x\r\n\r\n", HTTP_UNSUPPORTED_VERSION);
    error("GET / HTTP/1.1\nHost: x\n\n", HTTP_BAD_REQUEST);
    char *large = malloc(HTTP_HEADER_LIMIT + 1);
    assert(large);
    memset(large, 'x', HTTP_LINE_LIMIT);
    large[HTTP_LINE_LIMIT] = 0;
    error(large, HTTP_LINE_TOO_LONG);
    const char *prefix = "GET / HTTP/1.1\r\nHost: x\r\nX: ";
    strcpy(large, prefix);
    memset(large + strlen(prefix), 'a', HTTP_HEADER_LIMIT - strlen(prefix));
    large[HTTP_HEADER_LIMIT] = 0;
    error(large, HTTP_HEADERS_TOO_LARGE);
    free(large);

    /* Real blocking receive: force growth beyond the initial 4 KiB. */
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    const char *head = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5000\r\n\r\n";
    unsigned char body[5000] = {0};
    assert(write(sockets[0], head, strlen(head)) == (ssize_t)strlen(head));
    assert(write(sockets[0], body, sizeof(body)) == sizeof(body));
    http_buffer buffer = {0}; r = (http_request){0};
    assert(http_receive_request(sockets[1], &buffer, &r, http_now_ms() + HTTP_HEADER_TIMEOUT_MS) == HTTP_COMPLETE);
    assert(buffer.capacity > 4096 && r.consumed == strlen(head) + sizeof(body));
    assert(!memcmp(buffer.data + r.method.offset, "POST", 4));
    free(buffer.data);
    close(sockets[0]); close(sockets[1]);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(write(sockets[0], "GET /", 5) == 5);
    assert(shutdown(sockets[0], SHUT_WR) == 0);
    buffer = (http_buffer){0}; r = (http_request){0};
    assert(http_receive_request(sockets[1], &buffer, &r, http_now_ms() + HTTP_HEADER_TIMEOUT_MS) == HTTP_PARSE_ERROR);
    assert(r.error == HTTP_INCOMPLETE_REQUEST);
    free(buffer.data);
    close(sockets[0]); close(sockets[1]);
    test_header_timeout();
    test_body_policy();
    puts("HTTP body policy: GET skips body; POST fixed 10s deadline passed");
    puts("HTTP header timeout: idle 5s, trickle and expired deadline passed");
    puts("HTTP request: parser, binary body, boundaries, receive growth and EOF passed");
}
