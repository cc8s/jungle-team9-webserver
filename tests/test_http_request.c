#include "http_request.h"
#include <assert.h>
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

int main(void)
{
    const unsigned char data[] = "GET /file?q=1 HTTP/1.1\r\nhOsT: localhost\r\nContent-Length: 4\r\n\r\n\0\r\nxNEXT";
    http_request r = {0};
    size_t request_length = sizeof(data)-1-4;
    for (size_t n = 0; n < request_length; n++)
        assert(http_parse_request(data, n, &r) == HTTP_NEED_MORE);
    assert(http_parse_request(data, sizeof(data)-1, &r) == HTTP_COMPLETE);
    assert(r.content_length == 4 && r.consumed == request_length);
    assert(r.method.length == 3 && !memcmp(data + r.method.offset, "GET", 3));
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
    const char *head = "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5000\r\n\r\n";
    unsigned char body[5000] = {0};
    assert(write(sockets[0], head, strlen(head)) == (ssize_t)strlen(head));
    assert(write(sockets[0], body, sizeof(body)) == sizeof(body));
    http_buffer buffer = {0}; r = (http_request){0};
    assert(http_receive_request(sockets[1], &buffer, &r) == HTTP_COMPLETE);
    assert(buffer.capacity > 4096 && r.consumed == strlen(head) + sizeof(body));
    assert(!memcmp(buffer.data + r.method.offset, "GET", 3));
    free(buffer.data);
    close(sockets[0]); close(sockets[1]);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(write(sockets[0], "GET /", 5) == 5);
    assert(shutdown(sockets[0], SHUT_WR) == 0);
    buffer = (http_buffer){0}; r = (http_request){0};
    assert(http_receive_request(sockets[1], &buffer, &r) == HTTP_PARSE_ERROR);
    assert(r.error == HTTP_INCOMPLETE_REQUEST);
    free(buffer.data);
    close(sockets[0]); close(sockets[1]);
    puts("HTTP request: parser, binary body, boundaries, receive growth and EOF passed");
}
