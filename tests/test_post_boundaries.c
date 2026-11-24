#include "http_response.h"
#include "json.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { int fd; unsigned char *wire; size_t length, used; unsigned char *output; } client_job;
static void *client(void *argument)
{
    client_job *j = argument;
    size_t sent = 0;
    while (sent < j->length) {
        ssize_t n = write(j->fd, j->wire + sent, j->length - sent);
        assert(n > 0); sent += (size_t)n;
    }
    /* The server may close after replying before this half-close runs. */
    int stopped = shutdown(j->fd, SHUT_WR);
    assert(stopped == 0 || (stopped == -1 && errno == ENOTCONN));
    for (;;) {
        ssize_t n = read(j->fd, j->output + j->used, 2*HTTP_BODY_LIMIT - j->used);
        assert(n >= 0);
        if (!n) break;
        j->used += (size_t)n;
        assert(j->used < 2*HTTP_BODY_LIMIT);
    }
    close(j->fd);
    return NULL;
}

/* Exercise receive -> parse -> response with a concurrent client draining large output. */
static json_value *exchange(const char *path, const char *type, const unsigned char *body,
                            size_t length, int expected, http_request_error injected)
{
    unsigned char *wire = malloc(length + 512), *output = calloc(1, 2*HTTP_BODY_LIMIT + 1);
    assert(wire && output);
    int header = snprintf((char *)wire, 512, "POST %s HTTP/1.1\r\nHost: x\r\n%s%s%sContent-Length: %zu\r\n\r\n",
                          path, type ? "Content-Type: " : "", type ? type : "", type ? "\r\n" : "", length);
    assert(header > 0 && header < 512);
    if (length) memcpy(wire + header, body, length);
    int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
    /* Oversized requests are rejected at headers; do not send an unread body. */
    client_job job = {.fd=pair[1], .wire=wire, .length=(size_t)header + (length > HTTP_BODY_LIMIT ? 0 : length), .output=output};
    pthread_t worker; assert(pthread_create(&worker, NULL, client, &job) == 0);
    http_buffer buffer = {0}; http_request request = {0};
    http_parse_result result = http_receive_request(pair[0], &buffer, &request, http_now_ms()+5000);
    assert(result == (length > HTTP_BODY_LIMIT ? HTTP_PARSE_ERROR : HTTP_COMPLETE));
    if (injected) request.error = injected;
    assert(http_send_response(pair[0], &buffer, &request) == 0);
    close(pair[0]); assert(pthread_join(worker, NULL) == 0);
    int status; assert(sscanf((char *)output, "HTTP/1.1 %d", &status) == 1 && status == expected);
    char *end = strstr((char *)output, "\r\n\r\n"); assert(end);
    char *mime = strstr((char *)output, "Content-Type: application/json\r\n");
    assert(mime && mime < end);
    char *size = strstr((char *)output, "Content-Length: "); assert(size && size < end);
    size_t body_size = job.used - (size_t)(end + 4 - (char *)output);
    assert(strtoull(size+16, NULL, 10) == body_size);
    json_value *value = NULL;
    assert(json_parse((unsigned char *)end+4, body_size, &value) == 0);
    assert(json_member(value, "ok"));
    free(buffer.data); free(wire); free(output);
    return value;
}
static void expect(const char *path, const char *type, const char *body, int status)
{
    json_free(exchange(path, type, (const unsigned char *)body, strlen(body), status, HTTP_NO_ERROR));
}
static void test_json_escaping(void)
{
    unsigned char text[64]; size_t length = 0;
    text[length++] = '"'; text[length++] = '\\';
    for (unsigned int c=0; c<32; c++) text[length++] = (unsigned char)c;
    memcpy(text+length, "한글", strlen("한글")); length += strlen("한글");
    json_output body = {0};
    assert(json_append(&body, "{\"text\":", 8) == 0);
    assert(json_quote(&body, text, length) == 0);
    assert(json_append(&body, "}", 1) == 0);
    json_value *reply = exchange("/api/users", "application/json", (unsigned char *)body.data,
                                 body.length, 200, HTTP_NO_ERROR);
    const json_value *actual = json_member(json_member(reply, "data"), "text");
    assert(actual && actual->type == JSON_STRING && actual->text.length == length);
    assert(memcmp(actual->text.data, text, length) == 0);
    json_free(reply); free(body.data);
}
static void test_body_boundaries(void)
{
    unsigned char *body = malloc(HTTP_BODY_LIMIT+1); assert(body);
    memcpy(body, "{\"text\":\"", 9);
    memset(body+9, 'x', HTTP_BODY_LIMIT-11);
    memcpy(body+HTTP_BODY_LIMIT-2, "\"}", 2);
    json_value *reply = exchange("/api/users", "application/json", body, HTTP_BODY_LIMIT, 200, HTTP_NO_ERROR);
    assert(json_member(json_member(reply, "data"), "text")->text.length == HTTP_BODY_LIMIT-11);
    json_free(reply);
    json_free(exchange("/api/users", "application/json", body, HTTP_BODY_LIMIT+1, 413, HTTP_NO_ERROR));

    const char *prefix = "{\"filename\":\"max.bin\",\"content_type\":\"x\",\"encoding\":\"base64\",\"data\":\"";
    size_t start = strlen(prefix), encoded = (HTTP_BODY_LIMIT-start-2)/4*4;
    memcpy(body, prefix, start); memset(body+start, 'A', encoded);
    memcpy(body+start+encoded, "\"}", 2);
    memset(body+start+encoded+2, ' ', HTTP_BODY_LIMIT-start-encoded-2);
    reply = exchange("/api/upload", "application/json", body, HTTP_BODY_LIMIT, 201, HTTP_NO_ERROR);
    const json_value *data = json_member(reply, "data"), *id = json_member(data, "id");
    char path[256];
    snprintf(path, sizeof(path), "uploads_storage/%.*s.bin", (int)id->text.length, id->text.data);
    int fd = open(path, O_RDONLY); struct stat st;
    assert(fd >= 0 && fstat(fd, &st) == 0 && (size_t)st.st_size == encoded/4*3);
    unsigned char block[4096]; size_t total = 0; ssize_t got;
    while ((got = read(fd, block, sizeof(block))) > 0) {
        for (ssize_t i=0; i<got; i++) assert(block[i] == 0);
        total += (size_t)got;
    }
    assert(got == 0 && total == encoded/4*3 && close(fd) == 0);
    assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "uploads_storage/%.*s.json", (int)id->text.length, id->text.data);
    assert(unlink(path) == 0); json_free(reply);
    json_free(exchange("/api/upload", "application/json", body, HTTP_BODY_LIMIT+1, 413, HTTP_NO_ERROR));
    DIR *directory = opendir("uploads_storage"); assert(directory);
    struct dirent *entry;
    while ((entry=readdir(directory))) assert(entry->d_name[0] == '.');
    assert(closedir(directory) == 0); free(body);
}
int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    int original = open(".", O_RDONLY); assert(original >= 0);
    char temp[] = "/tmp/post-boundaries-XXXXXX";
    assert(mkdtemp(temp) && chdir(temp) == 0 && mkdir("uploads_storage", 0700) == 0);
    test_json_escaping(); test_body_boundaries();
    expect("/api/users", "application/json", "{}", 200);
    expect("/api/form", "application/x-www-form-urlencoded", "x=y", 200);
    expect("/api/users", "application/json", "", 400);
    expect("/api/form", "application/x-www-form-urlencoded", "", 400);
    expect("/api/upload", NULL, "", 400);
    expect("/api/users", "application/json", "{bad", 400);
    expect("/api/form", "application/x-www-form-urlencoded", "x=%zz", 400);
    expect("/api/upload", "application/json", "{}", 400);
    expect("/missing", "application/json", "{}", 404);
    const char *routes[] = {"/api/users", "/api/form", "/api/upload"};
    for (size_t i=0; i<3; i++) expect(routes[i], "text/plain", "x", 415);
    const struct {http_request_error error; int status;} errors[] = {
        {HTTP_BAD_REQUEST,400}, {HTTP_HEADER_TIMEOUT,408}, {HTTP_BODY_TIMEOUT,408},
        {HTTP_LENGTH_REQUIRED,411}, {HTTP_BODY_TOO_LARGE,413}, {HTTP_LINE_TOO_LONG,414},
        {HTTP_HEADERS_TOO_LARGE,431}, {HTTP_READ_ERROR,500}, {HTTP_NO_MEMORY,500},
        {HTTP_UNSUPPORTED_TRANSFER,501}, {HTTP_UNSUPPORTED_VERSION,505}
    };
    for (size_t i=0; i<sizeof(errors)/sizeof(errors[0]); i++)
        json_free(exchange("/api/users", "application/json", (const unsigned char *)"{}", 2,
                           errors[i].status, errors[i].error));
    assert(rmdir("uploads_storage") == 0 && fchdir(original) == 0 && close(original) == 0 && rmdir(temp) == 0);
    puts("POST boundaries: 1 MiB receive/upload, JSON escaping and success/error HTTP headers passed");
}
