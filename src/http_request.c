#include "http_request.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static http_parse_result fail(http_request *r, http_request_error error)
{
    r->error = error;
    return HTTP_PARSE_ERROR;
}

static int is_token(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c));
}

/* Return an index into data, never a pointer that realloc could invalidate. */
static size_t find_crlf(const unsigned char *data, size_t start, size_t length)
{
    for (size_t i = start; i + 1 < length; i++)
        if (data[i] == '\r' && data[i + 1] == '\n') return i;
    return length;
}

static int header_is(const unsigned char *data, size_t start, size_t length,
                     const char *name)
{
    return length == strlen(name) && !strncasecmp((const char *)data + start, name, length);
}

http_parse_result http_parse_request(const unsigned char *data, size_t length,
                                     http_request *r)
{
    if (r->error) return HTTP_PARSE_ERROR;
    if (!r->headers_complete) {
        /* Only inspect header bytes here; the body may contain arbitrary bytes. */
        size_t header_end = 0;
        for (size_t i = 0; i < length; i++) {
            unsigned char c = data[i];
            if (!c || (c < 32 && c != '\r' && c != '\n' && c != '\t') || c == 127)
                return fail(r, HTTP_BAD_REQUEST);
            if (c == '\n' && (!i || data[i-1] != '\r')) return fail(r, HTTP_BAD_REQUEST);
            if (c == '\r' && i + 1 < length && data[i+1] != '\n') return fail(r, HTTP_BAD_REQUEST);
            if (i >= 3 && !memcmp(data + i - 3, "\r\n\r\n", 4)) {
                header_end = i + 1;
                break;
            }
        }
        size_t line_end = find_crlf(data, 0, length);
        if ((line_end == length && length >= HTTP_LINE_LIMIT) ||
            (line_end != length && line_end + 2 > HTTP_LINE_LIMIT))
            return fail(r, HTTP_LINE_TOO_LONG);
        if (header_end > HTTP_HEADER_LIMIT || (!header_end && length >= HTTP_HEADER_LIMIT))
            return fail(r, HTTP_HEADERS_TOO_LARGE);
        if (!header_end) return HTTP_NEED_MORE;

        size_t first = 0;
        while (first < line_end && data[first] != ' ') first++;
        size_t second = first + 1;
        while (second < line_end && data[second] != ' ') second++;
        if (!first || first >= line_end || second == first + 1 || second >= line_end)
            return fail(r, HTTP_BAD_REQUEST);
        for (size_t i = 0; i < first; i++)
            if (!is_token(data[i])) return fail(r, HTTP_BAD_REQUEST);
        if (data[first + 1] != '/') return fail(r, HTTP_BAD_REQUEST);
        for (size_t i = first + 1; i < second; i++)
            if (data[i] <= 32 || data[i] == 127 || data[i] == '#') return fail(r, HTTP_BAD_REQUEST);
        if (line_end - second - 1 != 8 || memcmp(data + second + 1, "HTTP/", 5) ||
            data[second+6] < '0' || data[second+6] > '9' || data[second+7] != '.' ||
            data[second+8] < '0' || data[second+8] > '9') return fail(r, HTTP_BAD_REQUEST);
        if (memcmp(data + second + 1, "HTTP/1.1", 8)) return fail(r, HTTP_UNSUPPORTED_VERSION);
        r->method = (http_field){0, first};
        r->target = (http_field){first + 1, second - first - 1};
        r->version = (http_field){second + 1, 8};

        int has_length = 0, has_transfer = 0, has_host = 0;
        for (size_t start = line_end + 2; start < header_end - 2;) {
            size_t end = find_crlf(data, start, header_end), colon = start;
            while (colon < end && data[colon] != ':') {
                if (!is_token(data[colon])) return fail(r, HTTP_BAD_REQUEST);
                colon++;
            }
            if (colon == start || colon == end) return fail(r, HTTP_BAD_REQUEST);
            size_t value = colon + 1, tail = end;
            while (value < tail && (data[value] == ' ' || data[value] == '\t')) value++;
            while (tail > value && (data[tail-1] == ' ' || data[tail-1] == '\t')) tail--;
            http_field field = {value, tail - value};
            if (header_is(data, start, colon-start, "Host")) {
                if (has_host++ || !field.length) return fail(r, HTTP_BAD_REQUEST);
                for (size_t i = value; i < tail; i++)
                    if (data[i] <= 32 || strchr("/\\,@?#", data[i])) return fail(r, HTTP_BAD_REQUEST);
                r->host = field;
            } else if (header_is(data, start, colon-start, "Content-Length")) {
                if (has_length++ || !field.length) return fail(r, HTTP_BAD_REQUEST);
                size_t n = 0;
                for (size_t i = value; i < tail; i++) {
                    if (data[i] < '0' || data[i] > '9' || n > (SIZE_MAX - (data[i]-'0'))/10)
                        return fail(r, HTTP_BAD_REQUEST);
                    n = n * 10 + (data[i]-'0');
                }
                r->content_length = n;
            } else if (header_is(data, start, colon-start, "Transfer-Encoding")) {
                if (!field.length) return fail(r, HTTP_BAD_REQUEST);
                has_transfer = 1;
            } else if (header_is(data, start, colon-start, "Connection")) {
                r->connection = field;
            }
            start = end + 2;
        }
        if (!has_host || (has_length && has_transfer)) return fail(r, HTTP_BAD_REQUEST);
        if (has_transfer) return fail(r, HTTP_UNSUPPORTED_TRANSFER);
        if (r->content_length > HTTP_BODY_LIMIT) return fail(r, HTTP_BODY_TOO_LARGE);
        r->header_length = header_end;
        r->headers_complete = 1;
    }
    if (length < r->header_length + r->content_length) return HTTP_NEED_MORE;
    r->consumed = r->header_length + r->content_length;
    return HTTP_COMPLETE;
}

int64_t http_now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Recompute the remaining time after every wakeup, including EINTR. */
static http_request_error wait_for_header(int fd, int64_t deadline)
{
    for (;;) {
        int64_t now = http_now_ms();
        if (now < 0) return HTTP_READ_ERROR;
        int64_t left = deadline - now;
        if (left <= 0) return HTTP_HEADER_TIMEOUT;
        struct pollfd socket = {fd, POLLIN, 0};
        int result = poll(&socket, 1, left > INT_MAX ? INT_MAX : (int)left);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 || (socket.revents & POLLNVAL)) return HTTP_READ_ERROR;
        if (!result) continue;
        return HTTP_NO_ERROR;
    }
}

http_parse_result http_receive_request(int fd, http_buffer *buffer, http_request *r,
                                       int64_t header_deadline_ms)
{
    for (;;) {
        http_parse_result result = http_parse_request(buffer->data, buffer->length, r);
        if (result != HTTP_NEED_MORE) return result;
        if (buffer->length == buffer->capacity) {
            size_t capacity = buffer->capacity ? buffer->capacity * 2 : 4096;
            if (capacity > HTTP_REQUEST_LIMIT) capacity = HTTP_REQUEST_LIMIT;
            if (capacity <= buffer->capacity) return fail(r, HTTP_BODY_TOO_LARGE);
            void *grown = realloc(buffer->data, capacity);
            if (!grown) return fail(r, HTTP_NO_MEMORY);
            buffer->data = grown;
            buffer->capacity = capacity;
        }
        ssize_t n;
        if (!r->headers_complete) {
            http_request_error error = wait_for_header(fd, header_deadline_ms);
            if (error) return fail(r, error);
            /* Per-call nonblocking receive prevents a read from outliving the
               deadline; the socket itself keeps its original blocking mode. */
            n = recv(fd, buffer->data + buffer->length,
                     buffer->capacity - buffer->length, MSG_DONTWAIT);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            int64_t now = http_now_ms();
            if (now < 0) return fail(r, HTTP_READ_ERROR);
            if (now >= header_deadline_ms) return fail(r, HTTP_HEADER_TIMEOUT);
        } else {
            n = read(fd, buffer->data + buffer->length, buffer->capacity - buffer->length);
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return fail(r, HTTP_READ_ERROR);
        if (!n) return fail(r, HTTP_INCOMPLETE_REQUEST);
        buffer->length += (size_t)n;
    }
}
