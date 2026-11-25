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
    case 503: return "Service Unavailable";
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

static int prepare_headers(http_prepared_response *r, int status, const char *mime,
                           uintmax_t length, const char *allow)
{
    if (status >= 400) r->close_connection = 1;
    int n = snprintf(r->header, sizeof(r->header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %ju\r\n%s%s%s%s\r\n",
        status, reason(status), mime, length,
        r->close_connection ? "Connection: close\r\n" : "",
        allow ? "Allow: " : "", allow ? allow : "", allow ? "\r\n" : "");
    if (n < 0 || (size_t)n >= sizeof(r->header)) return -1;
    r->header_length = (size_t)n;
    return 0;
}
static int prepare_error(http_prepared_response *r, int status)
{
    int n = snprintf(r->fixed_body, sizeof(r->fixed_body), "%d %s\n", status, reason(status));
    if (n < 0 || (size_t)n >= sizeof(r->fixed_body)) return -1;
    r->body_length = (size_t)n;
    return prepare_headers(r, status, "text/plain; charset=utf-8", r->body_length,
                           status == 405 ? "GET" : NULL);
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

static int prepare_json(http_prepared_response *r, post_response *post, const char *allow)
{
    r->body = (unsigned char *)post->body.data;
    r->body_length = post->body.length;
    return prepare_headers(r, post->status, "application/json", r->body_length, allow);
}
static int prepare_json_error(http_prepared_response *r, int status, const char *allow)
{
    post_response post = {0};
    if (post_error(&post, status, "request_error", reason(status))) {
        free(post.body.data); return -1;
    }
    return prepare_json(r, &post, allow);
}
static int is_post(const http_buffer *buffer, const http_request *request)
{
    return buffer && buffer->data && field_in_buffer(request->method, buffer->length) &&
           request->method.length == 4 && !memcmp(buffer->data + request->method.offset, "POST", 4);
}

int http_prepare_response(const http_buffer *buffer, const http_request *request,
                          int force_close, http_prepared_response *state)
{
    *state = (http_prepared_response){.file = -1, .close_connection = force_close};
    if (!request)
        return prepare_error(state, 500);
    int status = request_status(request->error);
    int post = is_post(buffer, request);
    if (status != 200)
        return post ? prepare_json_error(state, status, NULL) : prepare_error(state, status);
    /* Error responses above do not require a receive buffer. */
    if (!buffer || !buffer->data || !field_in_buffer(request->method, buffer->length) ||
        !field_in_buffer(request->target, buffer->length) ||
        !field_in_buffer(request->version, buffer->length) ||
        !field_in_buffer(request->host, buffer->length) ||
        !field_in_buffer(request->connection, buffer->length) ||
        !field_in_buffer(request->content_type, buffer->length) || !request->method.length ||
        !request->target.length)
        return post ? prepare_json_error(state, 500, NULL) : prepare_error(state, 500);
    if (request->target.length >= HTTP_LINE_LIMIT)
        return post ? prepare_json_error(state, 414, NULL) : prepare_error(state, 414);
    char path[HTTP_LINE_LIMIT + 1];
    status = decode_path(buffer->data + request->target.offset, request->target.length, path,
                         sizeof(path));
    if (status)
        return post ? prepare_json_error(state, status, NULL) : prepare_error(state, status);
    if (post) {
        if (!request->has_content_length)
            return prepare_json_error(state, 411, NULL);
        if (request->header_length > buffer->length ||
            request->content_length > buffer->length - request->header_length)
            return prepare_json_error(state, 500, NULL);
        post_response response = {0};
        int error = post_prepare(
            path, buffer->data + request->header_length, request->content_length,
            buffer->data + request->content_type.offset, request->content_type.length, &response);
        if (error) {
            free(response.body.data);
            return prepare_json_error(state, 500, NULL);
        }
        return prepare_json(state, &response, NULL);
    }
    if (post_is_route(path))
        return prepare_json_error(state, 405, "POST");
    if (request->method.length != 3 || memcmp(buffer->data + request->method.offset, "GET", 3))
        return prepare_error(state, 405);
    get_response response = {.file = -1};
    status = get_prepare(path, &response);
    if (status != 200)
        return prepare_error(state, status);
    state->file = response.file;
    state->file_left = response.length;
    return prepare_headers(state, 200, response.mime, response.length, NULL);
}

void http_prepare_unavailable(http_prepared_response *response)
{
    *response = (http_prepared_response){.file = -1, .close_connection = 1};
    (void)prepare_error(response, 503);
}
void http_prepared_dispose(http_prepared_response *response)
{
    if (response->file >= 0) close(response->file);
    free(response->body);
    *response = (http_prepared_response){.file = -1};
}
int http_prepare_file_chunk(http_prepared_response *r)
{
    r->chunk_length = r->chunk_sent = 0;
    if (!r->file_left) return 0;
    size_t count = r->file_left < sizeof(r->chunk) ? (size_t)r->file_left : sizeof(r->chunk);
    ssize_t n;
    do { n = read(r->file, r->chunk, count); } while (n < 0 && errno == EINTR);
    if (n <= 0) return -1;
    r->chunk_length = (size_t)n;
    r->file_left -= (size_t)n;
    return 0;
}

http_response_result http_respond(int fd, const http_buffer *buffer,
                                  const http_request *request, int force_close)
{
    int64_t now = http_now_ms();
    if (now < 0)
        return (http_response_result){.io_error = -1, .close_connection = 1};
    http_prepared_response response;
    int result = http_prepare_response(buffer, request, force_close, &response);
    int64_t deadline = now + 5000;
    if (!result) result = http_send_all(fd, response.header, response.header_length, &deadline);
    if (!result && response.file >= 0) {
        get_response file = {.file = response.file, .length = response.file_left};
        result = get_send_body(fd, &file, &deadline);
    } else if (!result) {
        result = http_send_all(fd, response.body ? response.body : (unsigned char *)response.fixed_body,
                               response.body_length, &deadline);
    }
    int closing = response.close_connection || result < 0;
    http_prepared_dispose(&response);
    return (http_response_result){.io_error = result, .close_connection = closing};
}

/* One-shot compatibility entry point for existing direct response callers. */
int http_send_response(int fd, const http_buffer *buffer, const http_request *request)
{
    return http_respond(fd, buffer, request, 1).io_error;
}
