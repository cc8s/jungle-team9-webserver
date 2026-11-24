#include "http_request.h"
#include "http_response.h"
#include "http_post.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int64_t simulated_ms;
static int receive_mode, send_mode, peer_fd, send_calls, successful_sends;
static size_t receive_limit, send_limit, drain_length;
static int64_t receive_after_ms, poll_step;
static unsigned char drained[128];

static int controlled_clock(clockid_t clock, struct timespec *value)
{
    assert(clock == CLOCK_MONOTONIC);
    value->tv_sec = (time_t)(simulated_ms / 1000);
    value->tv_nsec = (long)(simulated_ms % 1000) * 1000000;
    return 0;
}
static int controlled_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    assert(count == 1 && timeout > 0);
    if (send_mode == 1) {
        /* The slow peer drains previously accepted bytes between simulated waits. */
        ssize_t n = recv(peer_fd, drained + drain_length, sizeof(drained)-drain_length, MSG_DONTWAIT);
        if (n > 0) drain_length += (size_t)n;
        else assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    }
    if (send_mode && poll_step >= timeout) {
        simulated_ms += timeout;
        return 0;
    }
    if (send_mode) simulated_ms += poll_step;
    return poll(fds, count, 0);
}
static ssize_t controlled_recv(int fd, void *data, size_t length, int flags)
{
    assert(receive_mode);
    if (length > receive_limit) length = receive_limit;
    ssize_t n = recv(fd, data, length, flags);
    assert(n > 0);
    simulated_ms = receive_after_ms; /* Preempted after actual recv success. */
    return n;
}
static ssize_t controlled_send(int fd, const void *data, size_t length, int flags)
{
    send_calls++;
    if (send_mode == 2 || (send_mode == 3 && successful_sends) ||
        (send_mode == 4 && send_calls > 1) || send_mode == 5 ||
        (send_mode == 6 && send_calls > 2)) {
        errno = send_mode >= 4 ? EPIPE : EAGAIN;
        return -1;
    }
    size_t limit = send_mode == 6 && send_calls == 1 ? SIZE_MAX : send_limit;
    if (length > limit) length = limit;
    ssize_t n = send(fd, data, length, flags);
    assert(n > 0);
    successful_sends++;
    return n;
}

/* Exercise the real loops with controlled time/I/O, without a production test API. */
#define clock_gettime controlled_clock
#define poll controlled_poll
#define recv controlled_recv
#define send controlled_send
#define http_now_ms timing_now_ms
#define http_parse_request timing_parse_request
#define http_receive_request timing_receive_request
#define http_send_all timing_send_all
#define http_respond timing_respond
#define http_send_response timing_send_response
#include "../src/http_request.c"
#include "../src/http_response.c"
#undef clock_gettime
#undef poll
#undef recv
#undef send
#undef http_now_ms
#undef http_parse_request
#undef http_receive_request
#undef http_send_all
#undef http_send_response

static void reset(void)
{
    simulated_ms = 0; receive_mode = send_mode = send_calls = successful_sends = 0;
    receive_limit = send_limit = SIZE_MAX; drain_length = 0; poll_step = 0;
}
static void socket_pair(int pair[2])
{
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    int flags = fcntl(pair[0], F_GETFL);
    assert(flags >= 0 && fcntl(pair[0], F_SETFL, flags | O_NONBLOCK) == 0);
    peer_fd = pair[1];
}
static void test_receive_crossing(int post, int complete)
{
    reset(); receive_mode = 1;
    const char *header = post ? "POST /api/users HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\n" : "";
    const char *incoming = post ? "{}" : "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t length = strlen(incoming);
    receive_limit = complete ? length : length-1;
    receive_after_ms = post ? 10001 : 101;
    http_buffer buffer = {0};
    if (post) {
        buffer.capacity = 4096; buffer.data = malloc(buffer.capacity); assert(buffer.data);
        buffer.length = strlen(header); memcpy(buffer.data, header, buffer.length);
    }
    int pair[2]; socket_pair(pair);
    assert(write(pair[1], incoming, length) == (ssize_t)length);
    http_request request = {0};
    assert(timing_receive_request(pair[0], &buffer, &request, 100) ==
           (complete ? HTTP_COMPLETE : HTTP_PARSE_ERROR));
    assert(buffer.length == strlen(header) + receive_limit);
    assert(!memcmp(buffer.data + strlen(header), incoming, receive_limit));
    if (complete) assert(request.error == HTTP_NO_ERROR && request.consumed == buffer.length);
    else assert(request.error == (post ? HTTP_BODY_TIMEOUT : HTTP_HEADER_TIMEOUT));
    free(buffer.data); close(pair[0]); close(pair[1]);
}
static void test_send_progress(void)
{
    int pair[2]; reset(); socket_pair(pair); send_mode = 1; send_limit = 3; poll_step = 4000;
    int64_t deadline = 5000;
    assert(timing_send_all(pair[0], "abcdefghi", 9, &deadline) == 0);
    assert(simulated_ms == 12000 && deadline == 17000 && successful_sends == 3);
    ssize_t n = read(pair[1], drained + drain_length, sizeof(drained)-drain_length);
    assert(n > 0); drain_length += (size_t)n;
    assert(drain_length == 9 && !memcmp(drained, "abcdefghi", 9));
    close(pair[0]); close(pair[1]);

    reset(); socket_pair(pair); send_mode = 2; poll_step = 1000; deadline = 5000;
    assert(timing_send_all(pair[0], "abc", 3, &deadline) == -1);
    assert(simulated_ms == 5000 && deadline == 5000 && successful_sends == 0);
    close(pair[0]); close(pair[1]);

    reset(); socket_pair(pair); send_mode = 3; send_limit = 1; poll_step = 1000; deadline = 5000;
    assert(timing_send_all(pair[0], "abc", 3, &deadline) == -1);
    assert(simulated_ms == 6000 && deadline == 6000 && successful_sends == 1);
    char byte; assert(read(pair[1], &byte, 1) == 1 && byte == 'a');
    close(pair[0]); close(pair[1]);
}
static void verify_and_remove_upload(void)
{
    DIR *directory = opendir("uploads_storage"); assert(directory);
    struct dirent *entry; int binaries = 0, metadata = 0;
    while ((entry = readdir(directory))) {
        if (entry->d_name[0] == '.') continue;
        char path[512]; snprintf(path, sizeof(path), "uploads_storage/%s", entry->d_name);
        struct stat info; assert(stat(path, &info) == 0 && (info.st_mode & 0777) == 0400);
        int fd = open(path, O_RDONLY); assert(fd >= 0);
        unsigned char content[1024]; ssize_t n = read(fd, content, sizeof(content)); assert(n >= 0);
        if (strstr(entry->d_name, ".bin")) {
            assert(n == 4 && !memcmp(content, "\0\r\n\xff", 4)); binaries++;
        } else {
            json_value *value = NULL; assert(json_parse(content, (size_t)n, &value) == 0);
            const json_value *name = json_member(value, "filename"), *size = json_member(value, "size");
            assert(name && name->text.length == 10 && !memcmp(name->text.data, "secret.bin", 10));
            assert(size && size->text.length == 1 && size->text.data[0] == '4');
            json_free(value); metadata++;
        }
        close(fd); assert(unlink(path) == 0);
    }
    assert(closedir(directory) == 0 && binaries == 1 && metadata == 1);
}
static void test_upload_send_failure(size_t limit, int stage)
{
    reset(); send_mode = stage == 1 ? 5 : stage == 2 ? 6 : 4; send_limit = limit;
    const char *body = "{\"filename\":\"secret.bin\",\"content_type\":\"x\",\"encoding\":\"base64\",\"data\":\"AA0K/w==\"}";
    unsigned char wire[512];
    int length = snprintf((char *)wire, sizeof(wire), "POST /api/upload HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
    http_buffer buffer = {.data=wire, .length=(size_t)length}; http_request request = {0};
    assert(timing_parse_request(wire, buffer.length, &request) == HTTP_COMPLETE);
    int pair[2]; socket_pair(pair);
    assert(timing_send_response(pair[0], &buffer, &request) == -1);
    /* Any attempt to append a second response would cause another send call. */
    assert(send_calls == (stage == 1 ? 1 : stage == 2 ? 3 : 2));
    close(pair[0]);
    char output[512] = {0}; ssize_t n = read(pair[1], output, sizeof(output)-1);
    assert(n >= 0);
    if (stage == 1) assert(n == 0);
    else {
        assert(n > 0);
        if (stage != 2 && limit != SIZE_MAX) assert(n == (ssize_t)limit);
        assert(!strncmp(output, "HTTP/1.1 201 Created", 19));
        assert(!strstr(output, "500") && !strstr(output + 1, "HTTP/1.1"));
        if (limit == SIZE_MAX) assert(strstr(output, "\r\n\r\n") && !strstr(output, "{\"ok\""));
        if (stage == 2) {
            char *body_start = strstr(output, "\r\n\r\n"); assert(body_start);
            body_start += 4;
            assert((size_t)(output+n-body_start) == limit && !memcmp(body_start, "{\"ok\":t", limit));
        }
    }
    close(pair[1]); verify_and_remove_upload();
}
int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    test_receive_crossing(0, 1); test_receive_crossing(0, 0);
    test_receive_crossing(1, 1); test_receive_crossing(1, 0);
    test_send_progress();
    int original = open(".", O_RDONLY); assert(original >= 0);
    char temp[] = "/tmp/transfer-timing-XXXXXX";
    assert(mkdtemp(temp) && chdir(temp) == 0 && mkdir("uploads_storage", 0700) == 0);
    test_upload_send_failure(0, 1);       /* First header send fails. */
    test_upload_send_failure(19, 0);      /* Header was partially transmitted. */
    test_upload_send_failure(SIZE_MAX, 0); /* Headers sent; first body send fails. */
    test_upload_send_failure(7, 2);        /* A prefix of the JSON body was sent. */
    assert(rmdir("uploads_storage") == 0 && fchdir(original) == 0 && close(original) == 0 && rmdir(temp) == 0);
    puts("Transfer timing: recv deadline crossing, progress renewal, idle expiry and saved upload preservation passed");
}
