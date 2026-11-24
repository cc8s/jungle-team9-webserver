#include "http_response.h"
#include "http_post.h"
#include "http_get.h"
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Progress means bytes accepted into the local socket send buffer. */
int http_send_all(int fd, const void *data, size_t length, int64_t *deadline)
{
    const unsigned char *p = data;
    while (length) {
        int64_t now = http_now_ms();
        if (now < 0 || now >= *deadline) return -1;
        struct pollfd socket = {fd, POLLOUT, 0};
        int ready = poll(&socket, 1, (int)(*deadline - now));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return -1;
        ssize_t n = send(fd, p, length, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return -1;
        p += n;
        length -= (size_t)n;
        now = http_now_ms();
        if (now < 0) return -1;
        *deadline = now + 5000;
    }
    return 0;
}

typedef struct {
    int64_t deadline;
    int close_connection;
} response_state;

static const char *reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 411: return "Length Required";
    case 415: return "Unsupported Media Type";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default: return "Internal Server Error";
    }
}

static int request_status(http_request_error error)
{
    switch (error) {
    case HTTP_NO_ERROR: return 200;
    case HTTP_LINE_TOO_LONG: return 414;
    case HTTP_HEADERS_TOO_LARGE: return 431;
    case HTTP_BODY_TOO_LARGE: return 413;
    case HTTP_LENGTH_REQUIRED: return 411;
    case HTTP_UNSUPPORTED_TRANSFER: return 501;
    case HTTP_UNSUPPORTED_VERSION: return 505;
    case HTTP_HEADER_TIMEOUT: case HTTP_BODY_TIMEOUT: return 408;
    case HTTP_READ_ERROR: case HTTP_NO_MEMORY: return 500;
    default: return 400;
    }
}

static int send_headers(int fd, int status, const char *mime, uintmax_t length, const char *allow,
                        response_state *state)
{
    char header[512];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %ju\r\n%s%s%s%s\r\n",
                     status, reason(status), mime, length,
                     state->close_connection ? "Connection: close\r\n" : "", allow ? "Allow: " : "",
                     allow ? allow : "", allow ? "\r\n" : "");
    if (n < 0 || (size_t)n >= sizeof(header))
        return -1;
    return http_send_all(fd, header, (size_t)n, &state->deadline);
}

static int send_error(int fd, int status, response_state *state)
{
    state->close_connection = 1;
    char body[128];
    int n = snprintf(body, sizeof(body), "%d %s\n", status, reason(status));
    if (send_headers(fd, status, "text/plain; charset=utf-8", (size_t)n, status == 405 ? "GET" : NULL, state)) return -1;
    return http_send_all(fd, body, (size_t)n, &state->deadline);
}

static int hex(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode_path(const unsigned char *target, size_t length, char *path,
                       size_t path_size)
{
    if (!path_size) return 414;
    size_t end = 0, out = 0;
    while (end < length && target[end] != '?') end++;
    for (size_t i = 0; i < end; i++) {
        unsigned char c = target[i];
        if (c == '%') {
            if (i + 2 >= end || hex(target[i+1]) < 0 || hex(target[i+2]) < 0) return 400;
            c = (unsigned char)(hex(target[i+1]) * 16 + hex(target[i+2]));
            i += 2;
        }
        if (!c || c < 32 || c == 127 || c == '\\') return 400;
        if (out >= path_size - 1) return 414;
        path[out++] = (char)c;
    }
    path[out] = 0;
    if (!out || path[0] != '/') return 400;
    for (char *p = path; *p;) {
        while (*p == '/') p++;
        char *endpart = p;
        while (*endpart && *endpart != '/') endpart++;
        if (endpart - p == 2 && p[0] == '.' && p[1] == '.') return 403;
        p = endpart;
    }
    if (!strcmp(path, "/")) {
        if (path_size < sizeof("/index.html")) return 414;
        memcpy(path, "/index.html", sizeof("/index.html"));
    }
    return 0;
}

static int field_in_buffer(http_field field, size_t length)
{
    return field.offset <= length && field.length <= length - field.offset;
}

static int send_json_response(int fd, post_response *response, const char *allow, response_state *state)
{
    if (response->status >= 400)
        state->close_connection = 1;
    int result = send_headers(fd, response->status, "application/json", response->body.length,
                              allow, state);
    if (!result)
        result = http_send_all(fd, response->body.data, response->body.length, &state->deadline);
    free(response->body.data);
    return result;
}

static int json_error(int fd, int status, const char *allow, response_state *state)
{
    post_response response = {0};
    if (post_error(&response, status, "request_error", reason(status))) {
        free(response.body.data);
        return -1;
    }
    return send_json_response(fd, &response, allow, state);
}

static int is_post(const http_buffer *buffer, const http_request *request)
{
    return buffer && buffer->data && field_in_buffer(request->method, buffer->length) &&
           request->method.length == 4 && !memcmp(buffer->data + request->method.offset, "POST", 4);
}

static int respond_once(int fd, const http_buffer *buffer, const http_request *request,
                        response_state *state)
{
    if (!request)
        return send_error(fd, 500, state);
    int status = request_status(request->error);
    int post = is_post(buffer, request);
    if (status != 200)
        return post ? json_error(fd, status, NULL, state) : send_error(fd, status, state);
    /* Error responses above do not require a receive buffer. */
    if (!buffer || !buffer->data || !field_in_buffer(request->method, buffer->length) ||
        !field_in_buffer(request->target, buffer->length) ||
        !field_in_buffer(request->version, buffer->length) ||
        !field_in_buffer(request->host, buffer->length) ||
        !field_in_buffer(request->connection, buffer->length) ||
        !field_in_buffer(request->content_type, buffer->length) || !request->method.length ||
        !request->target.length)
        return post ? json_error(fd, 500, NULL, state) : send_error(fd, 500, state);
    if (request->target.length >= HTTP_LINE_LIMIT)
        return post ? json_error(fd, 414, NULL, state) : send_error(fd, 414, state);
    char path[HTTP_LINE_LIMIT + 1];
    status = decode_path(buffer->data + request->target.offset, request->target.length, path,
                         sizeof(path));
    if (status)
        return post ? json_error(fd, status, NULL, state) : send_error(fd, status, state);
    if (post) {
        if (!request->has_content_length)
            return json_error(fd, 411, NULL, state);
        if (request->header_length > buffer->length ||
            request->content_length > buffer->length - request->header_length)
            return json_error(fd, 500, NULL, state);
        post_response response = {0};
        int error = post_prepare(
            path, buffer->data + request->header_length, request->content_length,
            buffer->data + request->content_type.offset, request->content_type.length, &response);
        if (error) {
            free(response.body.data);
            return json_error(fd, 500, NULL, state);
        }
        return send_json_response(fd, &response, NULL, state);
    }
    if (post_is_route(path))
        return json_error(fd, 405, "POST", state);
    if (request->method.length != 3 || memcmp(buffer->data + request->method.offset, "GET", 3))
        return send_error(fd, 405, state);
    get_response response = {.file = -1};
    status = get_prepare(path, &response);
    if (status != 200)
        return send_error(fd, status, state);
    int result = send_headers(fd, 200, response.mime, response.length, NULL, state);
    if (!result)
        result = get_send_body(fd, &response, &state->deadline);
    close(response.file);
    return result;
}

http_response_result http_respond(int fd, const http_buffer *buffer,
                                  const http_request *request, int force_close)
{
    int64_t now = http_now_ms();
    if (now < 0)
        return (http_response_result){.io_error = -1, .close_connection = 1};
    response_state state = {.deadline = now + 5000, .close_connection = force_close};
    int result = respond_once(fd, buffer, request, &state);
    return (http_response_result){.io_error = result,
        .close_connection = state.close_connection || result < 0};
}

/* One-shot compatibility entry point for existing direct response callers. */
int http_send_response(int fd, const http_buffer *buffer, const http_request *request)
{
    return http_respond(fd, buffer, request, 1).io_error;
}
