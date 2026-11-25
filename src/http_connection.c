#include "http_connection.h"
#include "http_request.h"
#include "http_response.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static int
wait_readable(int fd, int64_t deadline)
{
    for (;;) {
        int64_t now = http_now_ms();
        if (now < 0)
            return -1;
        int64_t left = deadline - now;
        if (left <= 0)
            return 0;
        struct pollfd socket = {fd, POLLIN, 0};
        int ready = poll(&socket, 1, left > INT_MAX ? INT_MAX : (int)left);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || (socket.revents & POLLNVAL))
            return -1;
        if (ready)
            return 1;
    }
}

/* Idle EOF/timeout are silent. A first byte starts the next header deadline. */
static int
wait_next_request(int fd)
{
    int64_t now = http_now_ms();
    if (now < 0)
        return 0;
    int64_t deadline = now + HTTP_IDLE_TIMEOUT_MS;
    for (;;) {
        if (wait_readable(fd, deadline) <= 0)
            return 0;
        unsigned char byte;
        ssize_t n = recv(fd, &byte, 1, MSG_PEEK);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        return n > 0;
    }
}

static void
consume(http_buffer *buffer, size_t length)
{
    buffer->length -= length;
    memmove(buffer->data, buffer->data + length, buffer->length);
}

/*
 * The GET response was already sent: failure must not produce another
 * response.
 */
static int
discard_get_body(int fd, http_buffer *buffer, size_t remaining)
{
    size_t buffered = remaining < buffer->length ? remaining : buffer->length;
    consume(buffer, buffered);
    remaining -= buffered;
    if (!remaining)
        return 0;
    int64_t now = http_now_ms();
    if (now < 0)
        return -1;
    int64_t deadline = now + HTTP_BODY_TIMEOUT_MS;
    unsigned char discard[16384];
    while (remaining) {
        if (wait_readable(fd, deadline) <= 0)
            return -1;
        size_t count = remaining < sizeof(discard) ? remaining : sizeof(discard);
        ssize_t n = recv(fd, discard, count, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n <= 0)
            return -1;
        remaining -= (size_t) n;
    }
    return 0;
}

void
handle_connection(int fd, int64_t accepted_at)
{
    http_buffer buffer = {0};
    for (unsigned count = 0; count < HTTP_CONNECTION_REQUEST_LIMIT; count++) {
        // 1. 버퍼 비어있으면 대기 
        if (count && !buffer.length && !wait_next_request(fd)){
            break;
        }
            
        int64_t start = count ? http_now_ms() : accepted_at;
        if (start < 0){
            break;
        }
        
        http_request request = {0};
        // 2. 버퍼에 있는 request 파싱 
        http_parse_result parsed = http_receive_request(fd, &buffer, &request, start + HTTP_HEADER_TIMEOUT_MS);
        
        if (parsed == HTTP_COMPLETE) {
            printf("request: %.*s %.*s (%zu bytes)\n",
                   (int)request.method.length, buffer.data + request.method.offset,
                   (int)request.target.length, buffer.data + request.target.offset,
                   request.consumed);
        } else {
            fprintf(stderr, "request error: %d\n", request.error);
        }
        int force_close = request.connection_close || count + 1 == HTTP_CONNECTION_REQUEST_LIMIT;

        // 3. 메서드 종류에 따라 분기 처리
        http_response_result response = http_respond(fd, &buffer, &request, force_close);
        if (response.io_error < 0) {
            fprintf(stderr, "response send failed\n");
            break;
        }
        if (response.close_connection || parsed != HTTP_COMPLETE)
            break;
        int get = request.method.length == 3 && !memcmp(buffer.data + request.method.offset, "GET", 3);
        consume(&buffer, request.consumed);
        if (get && discard_get_body(fd, &buffer, request.content_length) < 0)
            break;
    }
    free(buffer.data);
}
