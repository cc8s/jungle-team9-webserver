#include "http_connection.h"
#include "http_request.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct {
    int fd;
    int64_t accepted_at;
} connection_job;
typedef struct {
    int status, close;
    size_t length;
    char body[2048];
} response;
static void *
serve(void *argument)
{
    connection_job *job = argument;
    handle_connection(job->fd, job->accepted_at);
    close(job->fd);
    return NULL;
}
static int
start(connection_job *job, pthread_t * thread)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    int flags = fcntl(pair[0], F_GETFL);
    assert(flags >= 0 && !fcntl(pair[0], F_SETFL, flags | O_NONBLOCK));
    struct timeval timeout = {20, 0};
    assert(!setsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    *job = (connection_job){
        pair[0], http_now_ms()
    };
    assert(!pthread_create(thread, NULL, serve, job));
    return pair[1];
}
static void
send_bytes(int fd, const void *data, size_t length)
{
    const char *p = data;
    while (length) {
        ssize_t n = send(fd, p, length, 0);
        assert(n > 0);
        p += n;
        length -= (size_t) n;
    }
}
static void send_text(int fd, const char *text){
    send_bytes(fd, text, strlen(text));
}
/* Keep-alive responses end at Content-Length, never at EOF. */
static response
receive_response(int fd)
{
    char header[2048];
    size_t used = 0;
    while (used < 4 || memcmp(header + used - 4, "\r\n\r\n", 4)) {
        assert(used + 1 < sizeof(header));
        assert(recv(fd, header + used, 1, 0) == 1);
        used++;
    }
    header[used] = 0;
    response result = {0};
    assert(sscanf(header, "HTTP/1.1 %d", &result.status) == 1);
    char *length = strstr(header, "Content-Length: ");
    assert(length);
    result.length = strtoull(length + 16, NULL, 10);
    result.close = strstr(header, "Connection: close\r\n") != NULL;
    if (!result.close)
        assert(!strstr(header, "Connection:"));
    assert(result.length < sizeof(result.body));
    for (size_t at = 0; at < result.length;) {
        ssize_t n = recv(fd, result.body + at, result.length - at, 0);
        assert(n > 0);
        at += (size_t) n;
    }
    return result;
}
static void
finish(int fd, pthread_t thread)
{
    unsigned char byte;
    ssize_t n = recv(fd, &byte, 1, 0);
    assert(n == 0 || (n < 0 && errno == ECONNRESET));
    close(fd);
    assert(!pthread_join(thread, NULL));
}
static const char *get = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
static const char *last = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";

static void
test_mixed(void)
{
    connection_job job;
    pthread_t thread;
    int fd = start(&job, &thread);
    send_text(fd, get);
    response r = receive_response(fd);
    assert(r.status == 200 && !r.close && !strcmp(r.body, "ok"));
    for (int number = 1; number <= 2; number++) {
        char wire[256], expected[16];
        snprintf(wire, sizeof(wire), "POST /api/users HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: 7\r\n\r\n{\"n\":%d}", number);
        snprintf(expected, sizeof(expected), "\"n\":%d", number);
        send_text(fd, wire);
        r = receive_response(fd);
        assert(r.status == 200 && !r.close && strstr(r.body, expected));
    }
    send_text(fd, last);
    r = receive_response(fd);
    assert(r.status == 200 && r.close);
    finish(fd, thread);
}
static void
test_pipeline(void)
{
    connection_job job;
    pthread_t thread;
    int fd = start(&job, &thread);
    send_text(fd, "POST /api/form HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 3\r\n\r\nx=1GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    response r = receive_response(fd);
    assert(r.status == 200 && strstr(r.body, "\"x\":\"1\""));
    r = receive_response(fd);
    assert(r.status == 200 && r.close);
    finish(fd, thread);
    fd = start(&job, &thread);
    for (int i = 0; i < 101; i++)
        send_text(fd, get);
    for (int i = 0; i < 100; i++) {
        r = receive_response(fd);
        assert(r.status == 200 && r.close == (i == 99));
    }
    finish(fd, thread);
}
static void
test_get_drain(void)
{
    connection_job job;
    pthread_t thread;
    int fd = start(&job, &thread);
    send_text(fd, "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\n");
    response r = receive_response(fd);
    assert(r.status == 200 && !r.close);
    unsigned char body[] = {0, '\r', '\n', 'x'};
    send_bytes(fd, body, sizeof(body));
    send_text(fd, last);
    r = receive_response(fd);
    assert(r.status == 200 && r.close);
    finish(fd, thread);
    fd = start(&job, &thread);
    send_text(fd, "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nbodyGET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    r = receive_response(fd);
    assert(r.status == 200 && !r.close);
    r = receive_response(fd);
    assert(r.status == 200 && r.close);
    finish(fd, thread);
    fd = start(&job, &thread);
    send_text(fd, "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\nConnection: close\r\n\r\n");
    r = receive_response(fd);
    assert(r.status == 200 && r.close);
    finish(fd, thread);
    fd = start(&job, &thread);
    send_text(fd, "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\n");
    r = receive_response(fd);
    assert(r.status == 200 && !r.close);
    int64_t begin = http_now_ms();
    finish(fd, thread);
    assert(http_now_ms() - begin >= 10000 && http_now_ms() - begin < 12000);
    fd = start(&job, &thread);
    send_text(fd, "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nx");
    r = receive_response(fd);
    assert(r.status == 200 && !r.close);
    assert(!shutdown(fd, SHUT_WR));
    finish(fd, thread);         /* EOF while draining must not add a second
                                 * response. */
}
static void
test_errors_close(void)
{
    const struct {
        const char *wire;
        int status;
    } cases[] = {
        {"GET /missing HTTP/1.1\r\nHost: x\r\n\r\n", 404},
        {"GET /api/users HTTP/1.1\r\nHost: x\r\n\r\n", 405},
        {"POST /api/users HTTP/1.1\r\nHost: x\r\n\r\n", 411},
        {"POST /api/users HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx", 415},
        {"POST /api/users HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: 1\r\n\r\nx", 400}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        connection_job job;
        pthread_t thread;
        int fd = start(&job, &thread);
        send_text(fd, cases[i].wire);
        send_text(fd, get);
        response r = receive_response(fd);
        assert(r.status == cases[i].status && r.close);
        finish(fd, thread);
    }
    connection_job job;
    pthread_t thread;
    int fd = start(&job, &thread);
    send_text(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive, CLOSE\r\nConnection: keep-alive\r\n\r\n");
    response r = receive_response(fd);
    assert(r.status == 200 && r.close);
    finish(fd, thread);
}
static void
test_next_header(void)
{
    connection_job job;
    pthread_t thread;
    int fd = start(&job, &thread);
    send_text(fd, get);
    response r = receive_response(fd);
    assert(!r.close);
    int64_t start_time = http_now_ms();
    finish(fd, thread);
    assert(http_now_ms() - start_time >= 5000 && http_now_ms() - start_time < 7000);
    fd = start(&job, &thread);
    send_text(fd, get);
    r = receive_response(fd);
    assert(!r.close);
    send_text(fd, "GET / HTTP/1.1\r");
    struct timespec delay = {0, 20000000};
    nanosleep(&delay, NULL);
    send_text(fd, "\nHost: x\r\nConnection: close\r\n\r\n");
    r = receive_response(fd);
    assert(r.status == 200 && r.close);
    finish(fd, thread);
    fd = start(&job, &thread);
    send_text(fd, get);
    r = receive_response(fd);
    assert(!r.close);
    send_text(fd, "GET / HTTP/1.1\r\nHost: x\r\n");
    start_time = http_now_ms();
    r = receive_response(fd);
    assert(r.status == 408 && r.close);
    assert(http_now_ms() - start_time >= 5000 && http_now_ms() - start_time < 7000);
    finish(fd, thread);
}
int
main(void)
{
    signal(SIGPIPE, SIG_IGN);
    int original = open(".", O_RDONLY), output = dup(STDOUT_FILENO), null = open("/dev/null", O_WRONLY);
    assert(original >= 0 && output >= 0 && null >= 0);
    fflush(stdout);
    assert(dup2(null, STDOUT_FILENO) >= 0);
    close(null);
    char directory[] = "/tmp/persistent-XXXXXX";
    assert(mkdtemp(directory) && !chdir(directory));
    assert(!mkdir("www", 0700));
    int file = open("www/index.html", O_CREAT | O_WRONLY, 0600);
    assert(file >= 0 && write(file, "ok", 2) == 2);
    close(file);
    test_mixed();
    test_pipeline();
    test_get_drain();
    test_errors_close();
    test_next_header();
    assert(!unlink("www/index.html") && !rmdir("www") && !fchdir(original));
    close(original);
    rmdir(directory);
    fflush(stdout);
    assert(dup2(output, STDOUT_FILENO) >= 0);
    close(output);
    puts("Persistent: reuse, pipelining, GET drain, close/errors, idle/header deadlines and 100 requests passed");
}
