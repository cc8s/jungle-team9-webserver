#include "http_response.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* Progress means bytes accepted into the local socket send buffer. */
static int send_all(int fd, const void *data, size_t length, int64_t *deadline)
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
    case HTTP_UNSUPPORTED_TRANSFER: return 501;
    case HTTP_UNSUPPORTED_VERSION: return 505;
    case HTTP_HEADER_TIMEOUT: case HTTP_BODY_TIMEOUT: return 408;
    case HTTP_READ_ERROR: case HTTP_NO_MEMORY: return 500;
    default: return 400;
    }
}

static int send_headers(int fd, int status, const char *mime, uintmax_t length,
                        int64_t *deadline)
{
    char header[512];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %ju\r\nConnection: close\r\n%s\r\n",
        status, reason(status), mime, length, status == 405 ? "Allow: GET\r\n" : "");
    if (n < 0 || (size_t)n >= sizeof(header)) return -1;
    return send_all(fd, header, (size_t)n, deadline);
}

static int send_error(int fd, int status, int64_t *deadline)
{
    char body[128];
    int n = snprintf(body, sizeof(body), "%d %s\n", status, reason(status));
    if (send_headers(fd, status, "text/plain; charset=utf-8", (size_t)n, deadline)) return -1;
    return send_all(fd, body, (size_t)n, deadline);
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

static const char *get_mime_type(const char *path)
{
    static const struct { const char *ext, *mime; } types[] = {
        {".html", "text/html"}, {".css", "text/css"}, {".js", "text/javascript"},
        {".json", "application/json"}, {".png", "image/png"},
        {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}
    };
    const char *ext = strrchr(path, '.');
    for (size_t i = 0; ext && i < sizeof(types)/sizeof(types[0]); i++)
        if (!strcasecmp(ext, types[i].ext)) return types[i].mime;
    return "application/octet-stream";
}

static int file_status(int error)
{
    if (error == ENOENT) return 404;
    if (error == EACCES || error == EPERM || error == ELOOP || error == ENOTDIR) return 403;
    return 500;
}

/* Walk from www one component at a time; never follow a symlink. */
static int open_static(char *path, int *status)
{
    int directory = open("www", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0) { *status = 500; return -1; }
    char *part = path;
    while (*part == '/') part++;
    while (*part) {
        char *slash = strchr(part, '/');
        if (slash) *slash = 0;
        struct stat st;
        if (fstatat(directory, part, &st, AT_SYMLINK_NOFOLLOW) < 0) {
            *status = file_status(errno); close(directory); return -1;
        }
        if (S_ISLNK(st.st_mode)) { *status = 403; close(directory); return -1; }
        int next = openat(directory, part, O_RDONLY | O_NOFOLLOW | O_NONBLOCK |
                          (slash ? O_DIRECTORY : 0));
        int saved = errno;
        close(directory);
        if (next < 0) { *status = file_status(saved); return -1; }
        directory = next;
        if (!slash) return directory;
        part = slash + 1;
        while (*part == '/') part++;
    }
    *status = 403;
    close(directory);
    return -1;
}

static int field_in_buffer(http_field field, size_t length)
{
    return field.offset <= length && field.length <= length - field.offset;
}

int http_send_response(int fd, const http_buffer *buffer, const http_request *request)
{
    int64_t now = http_now_ms();
    if (now < 0) return -1;
    int64_t deadline = now + 5000;
    if (!request) return send_error(fd, 500, &deadline);
    int status = request_status(request->error);
    if (status != 200) return send_error(fd, status, &deadline);
    /* Error responses above do not require a receive buffer. */
    if (!buffer || !buffer->data ||
        !field_in_buffer(request->method, buffer->length) ||
        !field_in_buffer(request->target, buffer->length) ||
        !field_in_buffer(request->version, buffer->length) ||
        !field_in_buffer(request->host, buffer->length) ||
        !field_in_buffer(request->connection, buffer->length) ||
        !request->method.length || !request->target.length)
        return send_error(fd, 500, &deadline);
    if (request->target.length >= HTTP_LINE_LIMIT)
        return send_error(fd, 414, &deadline);
    if (request->method.length != 3 || memcmp(buffer->data + request->method.offset, "GET", 3))
        return send_error(fd, 405, &deadline);
    char path[HTTP_LINE_LIMIT + 1];
    status = decode_path(buffer->data + request->target.offset, request->target.length, path, sizeof(path));
    if (status) return send_error(fd, status, &deadline);
    const char *mime = get_mime_type(path);
    int file = open_static(path, &status);
    if (file < 0) return send_error(fd, status, &deadline);
    struct stat st;
    if (fstat(file, &st) < 0) status = 500;
    else if (!S_ISREG(st.st_mode)) status = 403;
    else if (st.st_size < 0) status = 500;
    if (status != 0) { close(file); return send_error(fd, status, &deadline); }
    int result = send_headers(fd, 200, mime, (uintmax_t)st.st_size, &deadline);
    off_t left = st.st_size;
    unsigned char chunk[16384];
    while (!result && left > 0) {
        size_t count = left > (off_t)sizeof(chunk) ? sizeof(chunk) : (size_t)left;
        ssize_t n = read(file, chunk, count);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { result = -1; break; }
        result = send_all(fd, chunk, (size_t)n, &deadline);
        left -= n;
    }
    close(file);
    return result;
}
