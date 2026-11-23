#include "http_response.h"
#include <assert.h>
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

static void *respond(void *argument)
{
    response_job *job = argument;
    job->result = http_send_response(job->fd, &job->buffer, &job->request);
    close(job->fd);
    return NULL;
}

static void check(const char *method, const char *target, int expected,
                  const unsigned char *body, size_t body_length)
{
    char wire[1024];
    int n = snprintf(wire, sizeof(wire), "%s %s HTTP/1.1\r\nHost: x\r\n\r\n", method, target);
    response_job job = {0};
    job.buffer.data = (unsigned char *)wire;
    job.buffer.length = (size_t)n;
    assert(http_parse_request(job.buffer.data, job.buffer.length, &job.request) == HTTP_COMPLETE);
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    job.fd = sockets[0];
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
    if (expected == 405) assert(strstr((char *)output, "Allow: GET\r\n"));
    if (expected == 200) {
        assert(used-header_length == body_length);
        assert(!memcmp(output+header_length, body, body_length));
        if (strstr(target, ".png")) assert(strstr((char *)output, "Content-Type: image/png\r\n"));
    }
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
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
    check("POST", "/", 405, NULL, 0);

    /* Each A error is converted into the agreed HTTP status. */
    const struct { http_request_error error; int status; } cases[] = {
        {HTTP_BAD_REQUEST,400}, {HTTP_LINE_TOO_LONG,414}, {HTTP_HEADERS_TOO_LARGE,431},
        {HTTP_BODY_TOO_LARGE,413}, {HTTP_UNSUPPORTED_TRANSFER,501},
        {HTTP_UNSUPPORTED_VERSION,505}, {HTTP_BODY_TIMEOUT,408}, {HTTP_HEADER_TIMEOUT,408}, {HTTP_NO_MEMORY,500}
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
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
    assert(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    unsigned char wire[] = "GET /large HTTP/1.1\r\nHost: x\r\n\r\n";
    http_buffer buffer = {.data = wire, .length = sizeof(wire)-1};
    http_request request = {0};
    assert(http_parse_request(wire, sizeof(wire)-1, &request) == HTTP_COMPLETE);
    int flags = fcntl(pair[0], F_GETFL);
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
