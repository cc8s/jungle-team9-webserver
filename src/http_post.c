#include "http_post.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int post_is_route(const char *path)
{
    return !strcmp(path, "/api/users") || !strcmp(path, "/api/form") ||
           !strcmp(path, "/api/upload");
}

int post_error(post_response *response, int status, const char *code, const char *message)
{
    free(response->body.data);
    response->body = (json_output){0};
    response->status = status;
    int error = json_append(&response->body, "{\"ok\":false,\"error\":{\"code\":", 28);
    if (!error)
        error = json_quote(&response->body, (const unsigned char *)code, strlen(code));
    if (!error)
        error = json_append(&response->body, ",\"message\":", 11);
    if (!error)
        error = json_quote(&response->body, (const unsigned char *)message, strlen(message));
    if (!error)
        error = json_append(&response->body, "}}", 2);
    return error;
}

/* Recognize one media type and optional UTF-8 charset, never silently ignore parameters. */
static int content_type(const unsigned char *data, size_t length, const char *expected)
{
    if (!data || !length)
        return 415;
    size_t end = 0;
    while (end < length && data[end] != ';')
        end++;
    size_t media = end;
    while (media && (data[media - 1] == ' ' || data[media - 1] == '\t'))
        media--;
    if (media != strlen(expected) || strncasecmp((const char *)data, expected, media))
        return 415;
    if (end == length)
        return 0;
    size_t at = end + 1;
    while (at < length && (data[at] == ' ' || data[at] == '\t'))
        at++;
    const char *prefix = "charset=";
    if (length - at < strlen(prefix) ||
        strncasecmp((const char *)data + at, prefix, strlen(prefix)))
        return 415;
    at += strlen(prefix);
    size_t tail = length;
    while (tail > at && (data[tail - 1] == ' ' || data[tail - 1] == '\t'))
        tail--;
    if (tail > at && data[at] == '"' && data[tail - 1] == '"') {
        at++;
        tail--;
    }
    return tail - at == 5 && !strncasecmp((const char *)data + at, "utf-8", 5) ? 0 : 415;
}

static int base64_digit(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static int base64_decode(json_bytes text, unsigned char **bytes, size_t *length)
{
    *bytes = NULL;
    *length = 0;
    if (text.length % 4)
        return 400;
    unsigned char *out = malloc(text.length ? text.length / 4 * 3 : 1);
    if (!out)
        return 500;
    size_t used = 0;
    for (size_t i = 0; i < text.length; i += 4) {
        int a = base64_digit(text.data[i]), b = base64_digit(text.data[i + 1]);
        int c = text.data[i + 2] == '=' ? -2 : base64_digit(text.data[i + 2]);
        int d = text.data[i + 3] == '=' ? -2 : base64_digit(text.data[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1 ||
            ((c == -2 || d == -2) && i + 4 != text.length) || (c == -2 && (d != -2 || (b & 15))) ||
            (c >= 0 && d == -2 && (c & 3))) {
            free(out);
            return 400;
        }
        out[used++] = (unsigned char)((a << 2) | (b >> 4));
        if (c >= 0) {
            out[used++] = (unsigned char)((b << 4) | (c >> 2));
            if (d >= 0)
                out[used++] = (unsigned char)((c << 6) | d);
        }
    }
    *bytes = out;
    *length = used;
    return 0;
}

static int write_file(int fd, const void *data, size_t length)
{
    const unsigned char *p = data;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return 500;
        p += n;
        length -= (size_t)n;
    }
    return 0;
}

static int metadata(json_output *out, const char *id, const json_value *filename,
                    const json_value *type, size_t size)
{
    int error = json_append(out, "{\"id\":", 6);
    if (!error)
        error = json_quote(out, (const unsigned char *)id, strlen(id));
    if (!error)
        error = json_append(out, ",\"filename\":", 12);
    if (!error)
        error = json_quote(out, filename->text.data, filename->text.length);
    if (!error)
        error = json_append(out, ",\"content_type\":", 16);
    if (!error)
        error = json_quote(out, type->text.data, type->text.length);
    char number[64];
    int n = snprintf(number, sizeof(number), ",\"size\":%zu}", size);
    if (!error)
        error = json_append(out, number, (size_t)n);
    return error;
}

/* O_EXCL reserves both names. Roll back only the files created by this call. */
static int store_upload(const unsigned char *bytes, size_t length, const json_value *filename,
                        const json_value *type, json_output *result)
{
    int directory = open("uploads_storage", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0)
        return 500;
    int file = -1, meta = -1, error = 500, own_file = 0, own_meta = 0;
    char id[96], bin_name[104] = {0}, meta_name[104] = {0};
    json_output info = {0};
    static unsigned long sequence;
    for (int attempt = 0; attempt < 1000; attempt++) {
        struct timespec now;
        struct tm utc;
        char stamp[32];
        if (clock_gettime(CLOCK_REALTIME, &now) < 0 || !gmtime_r(&now.tv_sec, &utc) ||
            !strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%S", &utc))
            goto cleanup;
        snprintf(id, sizeof(id), "%s.%09ldZ_%lu", stamp, now.tv_nsec, sequence++);
        snprintf(bin_name, sizeof(bin_name), "%s.bin", id);
        snprintf(meta_name, sizeof(meta_name), "%s.json", id);
        file = openat(directory, bin_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (file < 0) {
            if (errno == EEXIST)
                continue;
            goto cleanup;
        }
        own_file = 1;
        meta = openat(directory, meta_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (meta >= 0) {
            own_meta = 1;
            break;
        }
        int saved = errno;
        close(file);
        file = -1;
        unlinkat(directory, bin_name, 0);
        own_file = 0;
        if (saved != EEXIST)
            goto cleanup;
    }
    if (file < 0 || meta < 0)
        goto cleanup;
    if (metadata(&info, id, filename, type, length) || write_file(file, bytes, length) ||
        write_file(meta, info.data, info.length) || fchmod(file, 0400) < 0 ||
        fchmod(meta, 0400) < 0)
        goto cleanup;
    int close_file = close(file);
    file = -1;
    int close_meta = close(meta);
    meta = -1;
    if (close_file < 0 || close_meta < 0)
        goto cleanup;
    *result = info;
    info = (json_output){0};
    error = 0;
cleanup:
    if (file >= 0)
        close(file);
    if (meta >= 0)
        close(meta);
    if (error) {
        if (own_file)
            unlinkat(directory, bin_name, 0);
        if (own_meta)
            unlinkat(directory, meta_name, 0);
    }
    free(info.data);
    close(directory);
    return error;
}

static int upload(const json_value *root, json_output *data)
{
    const json_value *filename = json_member(root, "filename"),
                     *type = json_member(root, "content_type");
    const json_value *encoding = json_member(root, "encoding"), *text = json_member(root, "data");
    if (!filename || !type || !encoding || !text || filename->type != JSON_STRING ||
        type->type != JSON_STRING || !type->text.length || encoding->type != JSON_STRING ||
        text->type != JSON_STRING ||
        encoding->text.length != 6 || memcmp(encoding->text.data, "base64", 6))
        return 400;
    unsigned char *bytes;
    size_t length;
    int error = base64_decode(text->text, &bytes, &length);
    if (!error)
        error = store_upload(bytes, length, filename, type, data);
    free(bytes);
    return error;
}

int post_prepare(const char *path, const unsigned char *body, size_t length,
                 const unsigned char *type, size_t type_length, post_response *response)
{
    response->status = 200;
    if (!post_is_route(path))
        return post_error(response, 404, "not_found", "Unknown POST path");
    if (!length)
        return post_error(response, 400, "invalid_body", "Request body is required");
    int form = !strcmp(path, "/api/form");
    int error = content_type(type, type_length,
                             form ? "application/x-www-form-urlencoded" : "application/json");
    if (error)
        return post_error(response, error, "unsupported_media_type", "Unsupported Content-Type");
    json_value *root = NULL;
    error = form ? form_parse(body, length, &root) : json_parse(body, length, &root);
    if (!error && !form && root->type != JSON_OBJECT)
        error = 400;
    json_output data = {0};
    if (!error && !strcmp(path, "/api/upload")) {
        error = upload(root, &data);
        response->status = 201;
    } else if (!error)
        error = json_write(&data, root);
    json_free(root);
    if (error) {
        free(data.data);
        return post_error(response, error,
                          error == 413   ? "limit_exceeded"
                          : error == 500 ? "storage_error"
                                         : "invalid_body",
                          error == 500 ? "Internal processing failed" : "Invalid request body");
    }
    error = json_append(&response->body, "{\"ok\":true,\"data\":", 18);
    if (!error)
        error = json_append(&response->body, data.data, data.length);
    if (!error)
        error = json_append(&response->body, "}", 1);
    free(data.data);
    return error;
}
