#include "http_request.h"
#include "http_response.h"
#include "http_post.h"
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

static int crash_write;
static ssize_t interrupted_write(int fd, const void *data, size_t length)
{
    if (crash_write) {
        assert(length >= 4);
        assert(write(fd, data, length / 2) == (ssize_t)(length / 2));
        kill(getpid(), SIGKILL);
        _exit(99);
    }
    return write(fd, data, length);
}
/* Stop a real upload at a known write boundary, with no production hook/API. */
#define write interrupted_write
#define post_is_route stability_post_is_route
#define post_prepare stability_post_prepare
#define post_error stability_post_error
#include "../src/http_post.c"
#undef write
#undef post_is_route
#undef post_prepare
#undef post_error

static void nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}
static size_t fd_count(void)
{
    DIR *d = opendir("/dev/fd"); assert(d);
    size_t count = 0; struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') count++;
    assert(closedir(d) == 0); return count;
}
static size_t resident_bytes(void)
{
#ifdef __APPLE__
    struct mach_task_basic_info info; mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    assert(task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS);
    return (size_t)info.resident_size;
#else
    FILE *f = fopen("/proc/self/statm", "r"); assert(f);
    unsigned long total, resident; assert(fscanf(f, "%lu %lu", &total, &resident) == 2);
    assert(fclose(f) == 0); return resident * (size_t)sysconf(_SC_PAGESIZE);
#endif
}
static void cleanup_uploads(void)
{
    DIR *d = opendir("uploads_storage"); assert(d); struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[512]; snprintf(path, sizeof(path), "uploads_storage/%s", e->d_name);
        assert(unlink(path) == 0);
    }
    assert(closedir(d) == 0);
}
static void exchange(const char *method, const char *path, const char *body, int status)
{
    char wire[1024];
    int n = snprintf(wire, sizeof(wire), "%s %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s", method, path, strlen(body), body);
    int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0); nonblocking(pair[0]);
    assert(write(pair[1], wire, (size_t)n) == n);
    http_buffer buffer = {0}; http_request request = {0};
    assert(http_receive_request(pair[0], &buffer, &request, http_now_ms()+5000) == HTTP_COMPLETE);
    assert(http_send_response(pair[0], &buffer, &request) == 0); close(pair[0]); free(buffer.data);
    char output[4096]; size_t used = 0; ssize_t got;
    while ((got = read(pair[1], output+used, sizeof(output)-used-1)) > 0) used += (size_t)got;
    assert(got == 0); output[used] = 0; close(pair[1]);
    int actual; assert(sscanf(output, "HTTP/1.1 %d", &actual) == 1 && actual == status);
}
static void test_repetition(void)
{
    const char *upload = "{\"filename\":\"x\",\"content_type\":\"x\",\"encoding\":\"base64\",\"data\":\"AA0K/w==\"}";
    size_t baseline_fd = 0, baseline_rss = 0, peak = 0;
    for (int batch = 0; batch <= 10; batch++) {
        for (int i = 0; i < 100; i++) {
            exchange("GET", "/", "", 200);
            exchange("POST", "/api/users", "{\"x\":\"한글\"}", 200);
            exchange("POST", "/api/users", "{bad", 400);
            exchange("POST", "/api/upload", upload, 201);
            cleanup_uploads();
        }
        size_t fds = fd_count(), rss = resident_bytes();
        printf("stability batch=%d requests=%d fd=%zu rss_bytes=%zu\n", batch, (batch+1)*400, fds, rss);
        if (!batch) { baseline_fd = fds; baseline_rss = peak = rss; }
        assert(fds == baseline_fd);
        if (rss > peak) peak = rss;
    }
    /* RSS is a bounded-growth observation, not proof of absence of small leaks. */
    size_t budget = 32*1024*1024;
#ifdef __has_feature
#if __has_feature(address_sanitizer)
    budget = 256*1024*1024; /* ASan quarantine intentionally retains allocations. */
#endif
#endif
#ifdef __SANITIZE_ADDRESS__
    budget = 256*1024*1024;
#endif
    assert(peak <= baseline_rss + budget);
    printf("stability repeat: 4400 requests, FD unchanged, RSS peak growth=%zu bytes (budget %zu)\n", peak-baseline_rss, budget);
}
static void write_fixture(const char *path, size_t length)
{
    int fd = open(path, O_CREAT|O_EXCL|O_WRONLY, 0600); assert(fd >= 0);
    unsigned char block[4096];
    for (size_t i=0; i<sizeof(block); i++) block[i] = (unsigned char)i;
    for (size_t offset=0; offset<length; offset+=sizeof(block)) {
        size_t count = length-offset < sizeof(block) ? length-offset : sizeof(block);
        assert(write(fd, block, count) == (ssize_t)count);
    }
    assert(close(fd) == 0);
}
static void test_tcp(int slow)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0); assert(listener >= 0);
    struct sockaddr_in address = {.sin_family=AF_INET, .sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0 && listen(listener, 1) == 0);
    socklen_t size = sizeof(address); assert(getsockname(listener, (struct sockaddr *)&address, &size) == 0);
    int result_pipe[2]; assert(pipe(result_pipe) == 0);
    pid_t worker = fork(); assert(worker >= 0);
    if (!worker) {
        close(result_pipe[0]); int fd = accept(listener, NULL, NULL); assert(fd >= 0); close(listener);
        int small = slow ? 65536 : 4096; assert(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0); nonblocking(fd);
        http_buffer buffer = {0}; http_request request = {0};
        assert(http_receive_request(fd, &buffer, &request, http_now_ms()+5000) == HTTP_COMPLETE);
        int64_t start = http_now_ms(); int result = http_send_response(fd, &buffer, &request);
        int64_t report[] = {result, http_now_ms()-start};
        assert(write(result_pipe[1], report, sizeof(report)) == sizeof(report));
        free(buffer.data); close(fd); close(result_pipe[1]); _exit(0);
    }
    close(result_pipe[1]); close(listener);
    int client = socket(AF_INET, SOCK_STREAM, 0); assert(client >= 0);
    int small = slow ? 65536 : 4096; assert(setsockopt(client, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)) == 0);
    struct timeval timeout = {.tv_sec=10};
    assert(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    assert(connect(client, (struct sockaddr *)&address, sizeof(address)) == 0);
    const char *wire = slow ? "GET /slow.bin HTTP/1.1\r\nHost: localhost\r\n\r\n" : "GET /blocked.bin HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(write(client, wire, strlen(wire)) == (ssize_t)strlen(wire));
    if (slow) {
        unsigned char *output = malloc(2200000); assert(output); size_t used = 0; ssize_t n;
        struct timespec pause = {.tv_nsec=50000000};
        int64_t receive_start = http_now_ms();
        while ((n = read(client, output+used, 8192)) > 0) { used += (size_t)n; assert(used < 2190000 && http_now_ms()-receive_start < 60000); nanosleep(&pause, NULL); }
        assert(n == 0); output[used] = 0;
        char *end = strstr((char *)output, "\r\n\r\n"); assert(end);
        size_t header = (size_t)(end+4-(char *)output);
        printf("TCP received body bytes=%zu\n", used-header);
        assert(used-header == 2*1024*1024);
        for (size_t i=0; i<used-header; i++) assert(output[header+i] == (unsigned char)i);
        free(output);
    }
    struct pollfd ready = {.fd=result_pipe[0], .events=POLLIN}; assert(poll(&ready, 1, 60000) == 1);
    int64_t report[2]; assert(read(result_pipe[0], report, sizeof(report)) == sizeof(report));
    assert(report[0] == (slow ? 0 : -1));
    printf("TCP timing observation before bound: %lld ms\n", (long long)report[1]);
    assert(report[1] >= 5000 && report[1] < 60000);
    printf("stability TCP %s: result=%lld elapsed_ms=%lld\n", slow ? "slow-drain 2MiB" : "no-drain 8MiB", (long long)report[0], (long long)report[1]);
    close(client); close(result_pipe[0]); int status; assert(waitpid(worker, &status, 0) == worker && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void test_kill_observation(void)
{
    pid_t worker = fork(); assert(worker >= 0);
    if (!worker) {
        crash_write = 1;
        const char *body = "{\"filename\":\"interrupted.bin\",\"content_type\":\"x\",\"encoding\":\"base64\",\"data\":\"AA0K/w==\"}";
        post_response response = {0};
        stability_post_prepare("/api/upload", (const unsigned char *)body, strlen(body), (const unsigned char *)"application/json", 16, &response);
        _exit(99);
    }
    int status; assert(waitpid(worker, &status, 0) == worker && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    DIR *d = opendir("uploads_storage"); assert(d); struct dirent *e; int count = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[512]; snprintf(path, sizeof(path), "uploads_storage/%s", e->d_name);
        struct stat info; assert(stat(path, &info) == 0);
        if (strstr(e->d_name, ".bin")) assert(info.st_size == 2);
        else { assert(strstr(e->d_name, ".json")); assert(info.st_size == 0); }
        assert((info.st_mode & 0777) == 0600); count++;
    }
    assert(closedir(d) == 0 && count == 2);
    puts("stability SIGKILL observation: partial .bin=2 bytes, .json=0 bytes, both 0600; no atomicity/durability guarantee");
    cleanup_uploads();
}
int main(void)
{
    signal(SIGPIPE, SIG_IGN); setbuf(stdout, NULL);
    int original = open(".", O_RDONLY); assert(original >= 0);
    char temp[] = "/tmp/stability-XXXXXX"; assert(mkdtemp(temp) && chdir(temp) == 0);
    assert(mkdir("www", 0700) == 0 && mkdir("uploads_storage", 0700) == 0);
    write_fixture("www/index.html", 32); write_fixture("www/slow.bin", 2*1024*1024); write_fixture("www/blocked.bin", 8*1024*1024);
    test_repetition(); test_tcp(1); test_tcp(0); test_kill_observation();
    assert(unlink("www/index.html") == 0 && unlink("www/slow.bin") == 0 && unlink("www/blocked.bin") == 0);
    assert(rmdir("www") == 0 && rmdir("uploads_storage") == 0);
    assert(fchdir(original) == 0 && close(original) == 0 && rmdir(temp) == 0);
    puts("Stability: bounded repetition, actual TCP slow/no-drain and SIGKILL observation passed");
}
