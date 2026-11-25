#include "http_request.h"
#include "http_response.h"
#include "http_get.h"
#include "http_post.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum operation { NONE, POLL, RECV, SEND, READ, OPENAT, WRITE, CHMOD, CLOSE };
static enum operation fault;
static int fault_at, calls, injected, fault_errno, bin_fd, meta_fd;
static int collision_kind, collisions, read_chunk;
static time_t wall_time = 1700000000;
static char sentinel[256];
static int hit(enum operation operation)
{
    if (fault == operation && ++calls == fault_at) {
        injected++; errno = fault_errno; return 1;
    }
    return 0;
}
static int inject_poll(struct pollfd *fds, nfds_t count, int timeout)
{ if (hit(POLL)) return -1; return poll(fds, count, timeout); }
static ssize_t inject_recv(int fd, void *data, size_t length, int flags)
{ if (hit(RECV)) return -1; return recv(fd, data, length, flags); }
static ssize_t inject_send(int fd, const void *data, size_t length, int flags)
{ if (hit(SEND)) return -1; return send(fd, data, length, flags); }
static ssize_t inject_read(int fd, void *data, size_t length)
{
    if (hit(READ)) return -1;
    if (read_chunk && length > 3) length = 3;
    return read(fd, data, length);
}
static int inject_openat(int directory, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = (mode_t)va_arg(args, int); va_end(args); }
    if (hit(OPENAT)) return -1;
    if (collision_kind && !collisions &&
        ((collision_kind == 1 && strstr(path, ".bin")) ||
         (collision_kind == 2 && strstr(path, ".json")))) {
        int existing = openat(directory, path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        assert(existing >= 0 && write(existing, "keep", 4) == 4 && close(existing) == 0);
        snprintf(sentinel, sizeof(sentinel), "uploads_storage/%s", path);
        collisions++;
    }
    int fd = openat(directory, path, flags, mode);
    if (fd >= 0 && (flags & O_CREAT)) {
        if (strstr(path, ".bin")) bin_fd = fd;
        else if (strstr(path, ".json")) meta_fd = fd;
    }
    return fd;
}
static ssize_t inject_write(int fd, const void *data, size_t length)
{ if (hit(WRITE)) return -1; return write(fd, data, length); }
static int inject_fchmod(int fd, mode_t mode)
{ if (hit(CHMOD)) return -1; return fchmod(fd, mode); }
static int inject_close(int fd)
{
    /* Model close reporting an error after releasing the descriptor. */
    int affected = fd == bin_fd || fd == meta_fd;
    int result = close(fd);
    if (affected && hit(CLOSE)) return -1;
    return result;
}
static int inject_clock(clockid_t id, struct timespec *value)
{
    if (id != CLOCK_REALTIME) return clock_gettime(id, value);
    value->tv_sec = wall_time; value->tv_nsec = 123456789; return 0;
}

int fault_send_all(int fd, const void *data, size_t length, int64_t *deadline);

#define poll inject_poll
#define recv inject_recv
#define send inject_send
#define read inject_read
#define openat inject_openat
#define write inject_write
#define fchmod inject_fchmod
#define close inject_close
#define clock_gettime inject_clock
#define http_now_ms fault_now_ms
#define http_parse_request fault_parse_request
#define http_receive_request fault_receive_request
#define http_send_all fault_send_all
#define http_respond fault_respond
#define http_send_response fault_send_response
#define get_prepare fault_get_prepare
#define get_send_body fault_get_send_body
#define post_prepare fault_post_prepare
#define post_error fault_post_error
#define post_is_route fault_post_is_route
#include "../src/http_request.c"
#include "../src/http_get.c"
#include "../src/http_post.c"
#define http_prepare_response included_test_fault_recovery_http_prepare_response
#define http_prepare_unavailable included_test_fault_recovery_http_prepare_unavailable
#define http_prepare_file_chunk included_test_fault_recovery_http_prepare_file_chunk
#define http_prepared_dispose included_test_fault_recovery_http_prepared_dispose
#include "../src/http_response.c"
#undef poll
#undef recv
#undef send
#undef read
#undef openat
#undef write
#undef fchmod
#undef close
#undef clock_gettime
#undef http_now_ms
#undef http_parse_request
#undef http_receive_request
#undef http_send_all
#undef http_send_response
#undef get_prepare
#undef get_send_body
#undef post_prepare
#undef post_error
#undef post_is_route

static void configure(enum operation operation, int at, int error)
{
    fault = operation; fault_at = at; fault_errno = error; calls = injected = 0;
    bin_fd = meta_fd = -1; collision_kind = collisions = read_chunk = 0; sentinel[0] = 0;
}
static void pair(int sockets[2])
{
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    int flags = fcntl(sockets[0], F_GETFL);
    assert(flags >= 0 && fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK) == 0);
}
static void test_receive_fault(enum operation operation, int error)
{
    configure(operation, 1, error);
    int sockets[2]; pair(sockets);
    const char wire[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(write(sockets[1], wire, sizeof(wire)-1) == sizeof(wire)-1);
    http_buffer buffer = {0}; http_request request = {0};
    http_parse_result result = fault_receive_request(sockets[0], &buffer, &request, http_now_ms()+1000);
    assert(injected == 1);
    if (error == EINTR) assert(result == HTTP_COMPLETE && buffer.length == sizeof(wire)-1);
    else assert(result == HTTP_PARSE_ERROR && request.error == HTTP_READ_ERROR);
    free(buffer.data); close(sockets[0]); close(sockets[1]);
}
static void test_send_fault(enum operation operation, int error)
{
    configure(operation, 1, error);
    int sockets[2]; pair(sockets); int64_t deadline = http_now_ms()+1000;
    int result = fault_send_all(sockets[0], "bytes", 5, &deadline);
    assert(injected == 1 && result == (error == EINTR ? 0 : -1));
    close(sockets[0]);
    char body[16]; ssize_t n = read(sockets[1], body, sizeof(body));
    assert(n == (error == EINTR ? 5 : 0));
    if (n) assert(!memcmp(body, "bytes", 5));
    close(sockets[1]);
}
static void test_file_read(int error, int at)
{
    configure(READ, at, error); read_chunk = 1;
    int sockets[2]; pair(sockets);
    const unsigned char wire[] = "GET /data HTTP/1.1\r\nHost: x\r\n\r\n";
    http_buffer buffer = {.data=(unsigned char *)wire, .length=sizeof(wire)-1};
    http_request request = {0};
    assert(http_parse_request(wire, sizeof(wire)-1, &request) == HTTP_COMPLETE);
    assert(fault_send_response(sockets[0], &buffer, &request) == (error == EINTR ? 0 : -1));
    assert(injected == 1); close(sockets[0]);
    char output[512] = {0}; size_t used = 0; ssize_t n;
    while ((n = read(sockets[1], output+used, sizeof(output)-used-1)) > 0) used += (size_t)n;
    assert(n == 0); close(sockets[1]);
    char *body = strstr(output, "\r\n\r\n"); assert(body); body += 4;
    assert(!strncmp(output, "HTTP/1.1 200", 12) && !strstr(output+1, "HTTP/1.1"));
    size_t expected = error == EINTR ? 9 : (size_t)(at-1)*3;
    assert(used-(size_t)(body-output) == expected && !memcmp(body, "abcdefghi", expected));
}
static size_t file_count(void)
{
    DIR *directory = opendir("uploads_storage"); assert(directory);
    size_t count = 0; struct dirent *entry;
    while ((entry = readdir(directory))) if (entry->d_name[0] != '.') count++;
    assert(closedir(directory) == 0); return count;
}
static void verify_keep(const char *path)
{
    int fd = open(path, O_RDONLY); char value[8];
    assert(fd >= 0 && read(fd, value, sizeof(value)) == 4 && !memcmp(value, "keep", 4));
    assert(close(fd) == 0);
}
static void upload_request(int expected, char *identifier, size_t capacity)
{
    const char *body = "{\"filename\":\"x\",\"content_type\":\"x\",\"encoding\":\"base64\",\"data\":\"AA0K/w==\"}";
    post_response response = {0};
    assert(fault_post_prepare("/api/upload", (const unsigned char *)body, strlen(body),
                             (const unsigned char *)"application/json", 16, &response) == 0);
    assert(response.status == expected);
    json_value *root = NULL; assert(json_parse((unsigned char *)response.body.data, response.body.length, &root) == 0);
    if (expected == 201) {
        const json_value *id = json_member(json_member(root, "data"), "id");
        assert(id && id->text.length < capacity);
        memcpy(identifier, id->text.data, id->text.length); identifier[id->text.length] = 0;
    }
    json_free(root); free(response.body.data);
}
static void remove_pair(const char *identifier)
{
    char path[256]; snprintf(path, sizeof(path), "uploads_storage/%s.bin", identifier);
    int fd = open(path, O_RDONLY); unsigned char body[8]; struct stat info;
    assert(fd >= 0 && fstat(fd, &info) == 0 && (info.st_mode & 0777) == 0400);
    assert(read(fd, body, sizeof(body)) == 4 && !memcmp(body, "\0\r\n\xff", 4));
    assert(close(fd) == 0 && unlink(path) == 0);
    snprintf(path, sizeof(path), "uploads_storage/%s.json", identifier);
    assert(stat(path, &info) == 0 && (info.st_mode & 0777) == 0400 && unlink(path) == 0);
}
static void test_storage_fault(enum operation operation, int at, int error)
{
    configure(operation, at, error); char id[128];
    upload_request(error == EINTR ? 201 : 500, id, sizeof(id));
    assert(injected == 1);
    if (error == EINTR) remove_pair(id);
    assert(file_count() == 1); verify_keep("uploads_storage/keep.bin");
    if (bin_fd >= 0) assert(fcntl(bin_fd, F_GETFD) == -1 && errno == EBADF);
    if (meta_fd >= 0) assert(fcntl(meta_fd, F_GETFD) == -1 && errno == EBADF);
}
static void test_collision(int kind, int failure)
{
    configure(failure ? WRITE : NONE, 1, EIO); collision_kind = kind;
    char id[128]; upload_request(failure ? 500 : 201, id, sizeof(id));
    assert(collisions == 1 && file_count() == (failure ? 2 : 4));
    verify_keep(sentinel); verify_keep("uploads_storage/keep.bin");
    if (!failure) remove_pair(id);
    assert(unlink(sentinel) == 0 && file_count() == 1);
}
int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    test_receive_fault(POLL, EINTR); test_receive_fault(POLL, EIO);
    test_receive_fault(RECV, EINTR); test_receive_fault(RECV, ECONNRESET);
    test_send_fault(POLL, EINTR); test_send_fault(POLL, EIO);
    test_send_fault(SEND, EINTR); test_send_fault(SEND, EPIPE);
    int original = open(".", O_RDONLY); assert(original >= 0);
    char temp[] = "/tmp/fault-recovery-XXXXXX";
    assert(mkdtemp(temp) && chdir(temp) == 0 && mkdir("www", 0700) == 0 && mkdir("uploads_storage", 0700) == 0);
    int fd = open("www/data", O_CREAT | O_WRONLY, 0600);
    assert(fd >= 0 && write(fd, "abcdefghi", 9) == 9 && close(fd) == 0);
    fd = open("uploads_storage/keep.bin", O_CREAT | O_WRONLY, 0600);
    assert(fd >= 0 && write(fd, "keep", 4) == 4 && close(fd) == 0);
    test_file_read(EINTR, 1); test_file_read(EIO, 1); test_file_read(EIO, 2);
    test_storage_fault(OPENAT, 1, EACCES); test_storage_fault(OPENAT, 2, EIO);
    for (int at = 1; at <= 2; at++) {
        test_storage_fault(WRITE, at, EIO); test_storage_fault(WRITE, at, EINTR);
        test_storage_fault(CHMOD, at, EACCES); test_storage_fault(CLOSE, at, EIO);
    }
    for (int kind = 1; kind <= 2; kind++) { test_collision(kind, 0); test_collision(kind, 1); }
    configure(NONE, 1, 0); char a[128], b[128], c[128];
    upload_request(201, a, sizeof(a)); upload_request(201, b, sizeof(b));
    wall_time -= 3600; upload_request(201, c, sizeof(c));
    assert(strcmp(a, b) && strcmp(a, c) && strcmp(b, c));
    remove_pair(a); remove_pair(b); remove_pair(c);
    assert(file_count() == 1 && unlink("uploads_storage/keep.bin") == 0);
    assert(rmdir("uploads_storage") == 0 && unlink("www/data") == 0 && rmdir("www") == 0);
    assert(fchdir(original) == 0 && close(original) == 0 && rmdir(temp) == 0);
    puts("Fault recovery: EINTR/I/O failures, partial file response, upload rollback and ID collisions passed");
}
