#include "http_response.h"
#include "http_post.h"
#include <dirent.h>
#include <assert.h>
#include <stdint.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    int fd, result;
    http_buffer buffer;
    http_request request;
} response_job;

static void nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

static void *respond(void *argument)
{
    response_job *job = argument;
    job->result = http_send_response(job->fd, &job->buffer, &job->request);
    close(job->fd);
    return NULL;
}

static void check_headers(const char *method, const char *target, int expected,
                          const unsigned char *body, size_t body_length,
                          const char *mime, const char *allow)
{
    char wire[1024];
    int n = snprintf(wire, sizeof(wire), "%s %s HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", method, target);
    response_job job = {0};
    job.buffer.data = (unsigned char *)wire;
    job.buffer.length = (size_t)n;
    assert(http_parse_request(job.buffer.data, job.buffer.length, &job.request) == HTTP_COMPLETE);
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    nonblocking(sockets[0]);
    job.fd = sockets[0];
    int send_buffer = 4096;
    assert(setsockopt(job.fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, respond, &job) == 0);
    unsigned char output[100000];
    size_t used = 0;
    for (;;) {
        ssize_t got = read(sockets[1], output + used, sizeof(output)-used-1);
        assert(got >= 0);
        if (!got) break;
        used += (size_t)got;
        assert(used < sizeof(output)-1);
    }
    output[used] = 0;
    assert(pthread_join(worker, NULL) == 0 && job.result == 0);
    close(sockets[1]);
    int status;
    assert(sscanf((char *)output, "HTTP/1.1 %d", &status) == 1 && status == expected);
    char *end = strstr((char *)output, "\r\n\r\n");
    assert(end);
    size_t header_length = (size_t)(end + 4 - (char *)output);
    char *length_header = strstr((char *)output, "Content-Length: ");
    assert(length_header && strstr((char *)output, "Connection: close\r\n"));
    assert(strtoull(length_header + 16, NULL, 10) == used-header_length);
    if (mime) {
        char header[128];
        assert(snprintf(header, sizeof(header), "Content-Type: %s\r\n", mime) > 0);
        char *found = strstr((char *)output, header);
        assert(found && found < end);
    }
    if (allow) {
        char header[64];
        assert(snprintf(header, sizeof(header), "Allow: %s\r\n", allow) > 0);
        char *found = strstr((char *)output, header);
        assert(found && found < end);
        assert(!strstr(found + strlen(header), "Allow: "));
    }
    if (expected == 200) {
        assert(used-header_length == body_length);
        assert(!memcmp(output+header_length, body, body_length));
        if (strstr(target, ".png")) assert(strstr((char *)output, "Content-Type: image/png\r\n"));
    }
}

static void check(const char *method, const char *target, int expected,
                  const unsigned char *body, size_t body_length)
{
    check_headers(method, target, expected, body, body_length, NULL,
                  expected == 405 ? "POST" : NULL);
}

static void fixture(const char *path, const unsigned char *body, size_t length)
{
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && write(fd, body, length) == (ssize_t)length);
    assert(close(fd) == 0);
}

static void test_get_routes(void)
{
    const struct { const char *extension, *mime; } types[] = {
        {"html", "text/html"}, {"css", "text/css"}, {"js", "text/javascript"},
        {"json", "application/json"}, {"png", "image/png"},
        {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
        {"unknown", "application/octet-stream"}, {"CSS", "text/css"}
    };
    const unsigned char body[] = {0, 'x', '\r', '\n', 255};
    for (size_t i = 0; i < sizeof(types)/sizeof(types[0]); i++) {
        char path[128], target[128];
        snprintf(path, sizeof(path), "www/mime.%s", types[i].extension);
        snprintf(target, sizeof(target), "/mime.%s", types[i].extension);
        fixture(path, body, sizeof(body));
        check_headers("GET", target, 200, body, sizeof(body), types[i].mime, NULL);
        assert(unlink(path) == 0);
    }
    assert(mkdir("www/css", 0700) == 0);
    fixture("www/css/main.css", body, sizeof(body));
    check_headers("GET", "/css/main.css?v=1", 200, body, sizeof(body), "text/css", NULL);
    assert(unlink("www/css/main.css") == 0 && rmdir("www/css") == 0);

    const char *routes[] = {"/api/users", "/api/form", "/api/upload"};
    const char *methods[] = {"GET", "HEAD", "PUT", "PATCH", "DELETE", "OPTIONS",
                             "TRACE", "CONNECT", "CUSTOM"};
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); i++) {
        for (size_t j = 0; j < sizeof(methods)/sizeof(methods[0]); j++) {
            check_headers(methods[j], routes[i], 405, NULL, 0, "application/json", "POST");
            char encoded[128];
            snprintf(encoded, sizeof(encoded), "/%%61pi/%s?v=1", routes[i] + 5);
            check_headers(methods[j], encoded, 405, NULL, 0, "application/json", "POST");
        }
    }
    check_headers("PUT", "/index.html", 405, NULL, 0, NULL, "GET");

    /* Create real upload artifacts, then try direct, traversal and symlink URLs. */
    assert(mkdir("uploads_storage", 0700) == 0);
    const char upload[] = "{\"filename\":\"secret.bin\",\"content_type\":\"application/octet-stream\","
                          "\"encoding\":\"base64\",\"data\":\"AA0K/w==\"}";
    post_response response = {0};
    assert(post_prepare("/api/upload", (const unsigned char *)upload, strlen(upload),
                        (const unsigned char *)"application/json", 16, &response) == 0);
    assert(response.status == 201);
    free(response.body.data);
    assert(symlink("../uploads_storage", "www/uploads-link") == 0);
    DIR *directory = opendir("uploads_storage");
    assert(directory);
    struct dirent *entry;
    size_t count = 0;
    while ((entry = readdir(directory))) {
        if (entry->d_name[0] == '.') continue;
        char target[512], path[512];
        snprintf(target, sizeof(target), "/uploads_storage/%s", entry->d_name);
        check("GET", target, 404, NULL, 0);
        snprintf(target, sizeof(target), "/../uploads_storage/%s", entry->d_name);
        check("GET", target, 403, NULL, 0);
        snprintf(target, sizeof(target), "/%%2e%%2e/uploads_storage/%s", entry->d_name);
        check("GET", target, 403, NULL, 0);
        snprintf(target, sizeof(target), "/uploads-link/%s", entry->d_name);
        check("GET", target, 403, NULL, 0);
        snprintf(path, sizeof(path), "uploads_storage/%s", entry->d_name);
        assert(unlink(path) == 0);
        count++;
    }
    assert(count == 2 && closedir(directory) == 0);
    assert(unlink("www/uploads-link") == 0 && rmdir("uploads_storage") == 0);
}

static void api_error(const http_buffer *buffer, const http_request *request, int expected)
{
    int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    nonblocking(pair[0]);
    assert(http_send_response(pair[0], buffer, request) == 0);
    char output[512] = {0};
    assert(read(pair[1], output, sizeof(output)-1) > 0);
    int status;
    assert(sscanf(output, "HTTP/1.1 %d", &status) == 1 && status == expected);
    close(pair[0]); close(pair[1]);
}

static void test_api_bounds(void)
{
    unsigned char data[] = "GET/";
    http_buffer buffer = {.data=data, .length=4};
    http_request valid = {.method={0,3}, .target={3,1}};
    http_request r = valid;
    r.target.offset = SIZE_MAX; api_error(&buffer, &r, 500);
    r = valid; r.target.length = SIZE_MAX; api_error(&buffer, &r, 500);
    r = valid; r.method.offset = 3; api_error(&buffer, &r, 500);
    r = valid; r.host.offset = 5; api_error(&buffer, &r, 500);
    r = valid; r.target.length = 0; api_error(&buffer, &r, 500);
    api_error(NULL, &valid, 500); api_error(&buffer, NULL, 500);
    http_buffer empty = {0}; api_error(&empty, &valid, 500);
    r = (http_request){.error=HTTP_BAD_REQUEST};
    api_error(NULL, &r, 400); /* Errors never dereference request fields. */
    unsigned char *large = malloc(HTTP_LINE_LIMIT+4);
    assert(large); memset(large, 'a', HTTP_LINE_LIMIT+4); memcpy(large, "GET/", 4);
    buffer = (http_buffer){.data=large, .length=HTTP_LINE_LIMIT+4};
    r = valid; r.target.length = HTTP_LINE_LIMIT;
    api_error(&buffer, &r, 414);
    r.target.length++; api_error(&buffer, &r, 414);
    free(large);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    test_api_bounds();
    int original = open(".", O_RDONLY);
    char temp[] = "/tmp/http-response-XXXXXX";
    assert(mkdtemp(temp) && chdir(temp) == 0 && mkdir("www", 0700) == 0);
    int file = open("www/index.html", O_CREAT | O_WRONLY, 0600);
    assert(file >= 0 && write(file, "hello", 5) == 5); close(file);
    unsigned char bytes[65536];
    for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = (unsigned char)i;
    file = open("www/data.png", O_CREAT | O_WRONLY, 0600);
    assert(file >= 0 && write(file, bytes, sizeof(bytes)) == sizeof(bytes)); close(file);
    assert(mkdir("www/sub", 0700) == 0);
    assert(symlink("data.png", "www/link") == 0);
    assert(symlink("sub", "www/linked-dir") == 0);
    check("GET", "/", 200, (unsigned char *)"hello", 5);
    check("GET", "/data.png?q=1", 200, bytes, sizeof(bytes));
    check("GET", "/%64ata.png", 200, bytes, sizeof(bytes));
    check("GET", "/missing", 404, NULL, 0);
    check("GET", "/sub", 403, NULL, 0);
    check("GET", "/link", 403, NULL, 0);
    check("GET", "/linked-dir/file", 403, NULL, 0);
    check("GET", "/%2e%2e/secret", 403, NULL, 0);
    check("GET", "/%00", 400, NULL, 0);
    check("GET", "/%z0", 400, NULL, 0);
    check("POST", "/", 404, NULL, 0);
    check("GET", "/api/users", 405, NULL, 0);

    test_get_routes();

    /* Each A error is converted into the agreed HTTP status. */
    const struct { http_request_error error; int status; } cases[] = {
        {HTTP_LENGTH_REQUIRED,411}, {HTTP_BAD_REQUEST,400}, {HTTP_LINE_TOO_LONG,414}, {HTTP_HEADERS_TOO_LARGE,431},
        {HTTP_BODY_TOO_LARGE,413}, {HTTP_UNSUPPORTED_TRANSFER,501},
        {HTTP_UNSUPPORTED_VERSION,505}, {HTTP_BODY_TIMEOUT,408}, {HTTP_HEADER_TIMEOUT,408}, {HTTP_NO_MEMORY,500}
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        nonblocking(pair[0]);
        http_request request = {.error = cases[i].error};
        http_buffer buffer = {0};
        assert(http_send_response(pair[0], &buffer, &request) == 0);
        char result[512] = {0};
        assert(read(pair[1], result, sizeof(result)-1) > 0);
        int status; assert(sscanf(result, "HTTP/1.1 %d", &status) == 1 && status == cases[i].status);
        close(pair[0]); close(pair[1]);
    }
    /* A reader that never drains its socket must cause a 5-second write timeout. */
    file = open("www/large", O_CREAT | O_WRONLY, 0600);
    assert(file >= 0 && ftruncate(file, 4*1024*1024) == 0); close(file);
    int pair[2], small = 4096;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    nonblocking(pair[0]);
    assert(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    unsigned char wire[] = "GET /large HTTP/1.1\r\nHost: x\r\n\r\n";
    http_buffer buffer = {.data = wire, .length = sizeof(wire)-1};
    http_request request = {0};
    assert(http_parse_request(wire, sizeof(wire)-1, &request) == HTTP_COMPLETE);
    int flags = fcntl(pair[0], F_GETFL);
    assert(flags & O_NONBLOCK);
    int64_t start = http_now_ms();
    assert(http_send_response(pair[0], &buffer, &request) == -1);
    int64_t elapsed = http_now_ms()-start;
    assert(elapsed >= 5000 && elapsed < 7000);
    assert(fcntl(pair[0], F_GETFL) == flags);
    close(pair[1]);
    assert(http_send_response(pair[0], &buffer, &request) == -1);
    close(pair[0]);
    unlink("www/large"); unlink("www/link"); unlink("www/linked-dir");
    unlink("www/index.html"); unlink("www/data.png");
    rmdir("www/sub"); rmdir("www");
    assert(fchdir(original) == 0); close(original); rmdir(temp);
    puts("HTTP response: static/binary, paths, error mapping, write timeout and disconnect passed");
}
