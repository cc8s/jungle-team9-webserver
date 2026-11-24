#include "http_post.h"
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static json_value *run(const char *path, const char *body, const char *type, int status)
{
    post_response response = {0};
    assert(!post_prepare(path, (const unsigned char *)body, strlen(body),
                         (const unsigned char *)type, type ? strlen(type) : 0, &response));
    assert(response.status == status);
    json_value *value = NULL;
    assert(!json_parse((unsigned char *)response.body.data, response.body.length, &value));
    free(response.body.data);
    return value;
}
static void expect(const char *path, const char *body, const char *type, int status)
{
    json_free(run(path, body, type, status));
}
static size_t upload_file_count(void)
{
    DIR *directory = opendir("uploads_storage");
    assert(directory);
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (entry->d_name[0] != '.') count++;
    assert(closedir(directory) == 0);
    return count;
}

static void assert_text(const json_value *value, const unsigned char *bytes, size_t length)
{
    assert(value && value->type == JSON_STRING && value->text.length == length);
    assert(!memcmp(value->text.data, bytes, length));
}

static void stored_upload(const char *body, const unsigned char *bytes, size_t length,
                          const unsigned char *filename, size_t filename_length,
                          const unsigned char *type, size_t type_length)
{
    size_t before = upload_file_count();
    json_value *response = run("/api/upload", body, "application/json", 201);
    const json_value *data = json_member(response, "data");
    const json_value *id = json_member(data, "id");
    assert(id && id->text.length < 100);
    assert_text(json_member(data, "filename"), filename, filename_length);
    char identifier[100], size[32];
    memcpy(identifier, id->text.data, id->text.length);
    identifier[id->text.length] = 0;
    snprintf(size, sizeof(size), "%zu", length);
    const json_value *reported = json_member(data, "size");
    assert(reported && reported->type == JSON_NUMBER && reported->text.length == strlen(size));
    assert(!memcmp(reported->text.data, size, strlen(size)));
    json_free(response);
    char bin[160], meta[160];
    snprintf(bin, sizeof(bin), "uploads_storage/%s.bin", identifier);
    snprintf(meta, sizeof(meta), "uploads_storage/%s.json", identifier);
    struct stat st;
    assert(stat(bin, &st) == 0 && st.st_size == (off_t)length && (st.st_mode & 0777) == 0400);
    assert(stat(meta, &st) == 0 && (st.st_mode & 0777) == 0400);
    unsigned char contents[2048];
    int fd = open(bin, O_RDONLY);
    assert(fd >= 0 && read(fd, contents, sizeof(contents)) == (ssize_t)length);
    assert(!memcmp(contents, bytes, length) && close(fd) == 0);
    fd = open(meta, O_RDONLY);
    assert(fd >= 0);
    ssize_t got = read(fd, contents, sizeof(contents));
    assert(got > 0 && close(fd) == 0);
    json_value *metadata = NULL;
    assert(!json_parse(contents, (size_t)got, &metadata));
    assert_text(json_member(metadata, "filename"), filename, filename_length);
    assert_text(json_member(metadata, "content_type"), type, type_length);
    reported = json_member(metadata, "size");
    assert(reported && reported->type == JSON_NUMBER && reported->text.length == strlen(size));
    assert(!memcmp(reported->text.data, size, strlen(size)));
    assert_text(json_member(metadata, "id"), (unsigned char *)identifier, strlen(identifier));
    json_free(metadata);
    assert(upload_file_count() == before + 2);
    assert(unlink(bin) == 0 && unlink(meta) == 0);
    assert(upload_file_count() == before);
}

static void test_upload_inputs(void)
{
    int original = open(".", O_RDONLY);
    char temp[] = "/tmp/post-inputs-XXXXXX";
    assert(original >= 0 && mkdtemp(temp) && chdir(temp) == 0);
    assert(mkdir("uploads_storage", 0700) == 0);
    /* Every field must be a string; rejection must leave storage untouched. */
    const char *wrong[] = {"null", "true", "123", "[]", "{}"};
    for (size_t field = 0; field < 4; field++) {
        for (size_t kind = 0; kind < sizeof(wrong)/sizeof(wrong[0]); kind++) {
            const char *values[] = {"\"x\"", "\"image/png\"", "\"base64\"", "\"AAEC\""};
            values[field] = wrong[kind];
            char body[512];
            snprintf(body, sizeof(body), "{\"filename\":%s,\"content_type\":%s,\"encoding\":%s,\"data\":%s}",
                     values[0], values[1], values[2], values[3]);
            expect("/api/upload", body, "application/json", 400);
            assert(upload_file_count() == 0);
        }
    }
    const char *encodings[] = {"", "BASE64", "base64url", "utf-8", "base64 ", "base64\\u0000", "base"};
    for (size_t i = 0; i < sizeof(encodings)/sizeof(encodings[0]); i++) {
        char body[512];
        snprintf(body, sizeof(body), "{\"filename\":\"x\",\"content_type\":\"x\",\"encoding\":\"%s\",\"data\":\"AAEC\"}", encodings[i]);
        expect("/api/upload", body, "application/json", 400);
        assert(upload_file_count() == 0);
    }
    const unsigned char binary[] = {0, 1, 2};
    stored_upload("{\"filename\":\"photo.jpg\",\"content_type\":\"image/png\",\"encoding\":\"base64\",\"data\":\"AAEC\"}",
                  binary, sizeof(binary), (unsigned char *)"photo.jpg", 9, (unsigned char *)"image/png", 9);
    /* Empty data is different from an HTTP body of length zero: it creates a pair. */
    stored_upload("{\"filename\":\"empty\",\"content_type\":\"x\",\"encoding\":\"base64\",\"data\":\"\"}",
                  binary, 0, (unsigned char *)"empty", 5, (unsigned char *)"x", 1);
    stored_upload("{\"filename\":\"nul\\u0000name\",\"content_type\":\"x\\u0000y\",\"encoding\":\"base64\",\"data\":\"AAEC\"}",
                  binary, sizeof(binary), (unsigned char *)"nul\0name", 8, (unsigned char *)"x\0y", 3);
    /* Names referring to existing files must remain metadata, without overwriting them. */
    int fd = open("uploads_storage/existing.bin", O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && write(fd, "keep", 4) == 4 && close(fd) == 0);
    stored_upload("{\"filename\":\"existing.bin\",\"content_type\":\"x\",\"encoding\":\"base64\",\"data\":\"AAEC\"}",
                  binary, sizeof(binary), (unsigned char *)"existing.bin", 12, (unsigned char *)"x", 1);
    char absolute[256], body[512];
    snprintf(absolute, sizeof(absolute), "%s/uploads_storage/existing.bin", temp);
    snprintf(body, sizeof(body), "{\"filename\":\"%s\",\"content_type\":\"x\",\"encoding\":\"base64\",\"data\":\"AAEC\"}", absolute);
    stored_upload(body, binary, sizeof(binary), (unsigned char *)absolute, strlen(absolute), (unsigned char *)"x", 1);
    char sentinel[8];
    fd = open("uploads_storage/existing.bin", O_RDONLY);
    assert(fd >= 0 && read(fd, sentinel, sizeof(sentinel)) == 4 && !memcmp(sentinel, "keep", 4));
    assert(close(fd) == 0 && upload_file_count() == 1);
    assert(unlink("uploads_storage/existing.bin") == 0 && rmdir("uploads_storage") == 0);
    assert(fchdir(original) == 0 && close(original) == 0 && rmdir(temp) == 0);
    puts("POST upload inputs: 27 invalid combinations and 5 byte/metadata storage cases passed");
}

int main(void)
{
    test_upload_inputs();
    expect("/api/users", "{\"name\":\"홍길동\",\"age\":25}", "application/json", 200);
    expect("/api/users", "{}", "Application/JSON; charset=\"UTF-8\"", 200);
    expect("/api/users", "{}", "application/json; charset=latin1", 415);
    expect("/api/users", "{}", "application/json; foo=bar", 415);
    expect("/api/users", "{}", NULL, 415);
    expect("/api/users", "[]", "application/json", 400);
    expect("/api/users", "{bad}", "application/json", 400);
    expect("/api/form", "name=hello+world&age=25", "application/x-www-form-urlencoded", 200);
    expect("/api/form", "name=a&name=b", "application/x-www-form-urlencoded", 400);
    expect("/unknown", "", "application/json", 404);
    const char *paths[] = {"/api/users", "/api/form", "/api/upload"};
    const char *types[] = {NULL, "", "application/json", "application/x-www-form-urlencoded"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        for (size_t j = 0; j < sizeof(types) / sizeof(types[0]); j++) {
            json_value *response = run(paths[i], "", types[j], 400);
            const json_value *error = json_member(response, "error");
            assert_text(json_member(error, "code"), (const unsigned char *)"invalid_body", 12);
            json_free(response);
        }
    }
    expect("/api/upload", "{}", "multipart/form-data; boundary=x", 415);

    int original = open(".", O_RDONLY);
    char temp[] = "/tmp/post-upload-XXXXXX";
    assert(mkdtemp(temp) && chdir(temp) == 0);
    const char *good = "{\"filename\":\"../홍길동.png\",\"content_type\":\"image/"
                       "png\",\"encoding\":\"base64\",\"data\":\"AA0K/w==\"}";
    expect("/api/upload", good, "application/json", 500); /* Server never creates the directory. */
    assert(mkdir("uploads_storage", 0700) == 0);
    json_value *response = run("/api/upload", good, "application/json", 201);
    const json_value *data = json_member(response, "data"), *id = json_member(data, "id");
    assert(id && id->text.length < 100);
    char identifier[100];
    memcpy(identifier, id->text.data, id->text.length);
    identifier[id->text.length] = 0;
    assert(json_member(data, "size")->text.length == 1 &&
           json_member(data, "size")->text.data[0] == '4');
    json_free(response);
    char bin[160], meta[160];
    snprintf(bin, sizeof(bin), "uploads_storage/%s.bin", identifier);
    snprintf(meta, sizeof(meta), "uploads_storage/%s.json", identifier);
    struct stat st;
    assert(!stat(bin, &st) && (st.st_mode & 0777) == 0400 && st.st_size == 4);
    assert(!stat(meta, &st) && (st.st_mode & 0777) == 0400);
    int fd = open(bin, O_RDONLY);
    unsigned char bytes[8];
    assert(fd >= 0 && read(fd, bytes, sizeof(bytes)) == 4 && !memcmp(bytes, "\0\r\n\xff", 4));
    close(fd);
    fd = open(meta, O_RDONLY);
    char info[1024];
    ssize_t n = read(fd, info, sizeof(info));
    assert(n > 0);
    close(fd);
    json_value *metadata = NULL;
    assert(!json_parse((unsigned char *)info, (size_t)n, &metadata));
    assert(json_member(metadata, "filename")->text.length == strlen("../홍길동.png"));
    json_free(metadata);
    response = run("/api/upload", good, "application/json", 201);
    const json_value *second = json_member(json_member(response, "data"), "id");
    assert(second->text.length != strlen(identifier) ||
           memcmp(second->text.data, identifier, second->text.length));
    json_free(response);
    const char *bad[] = {"A",    "AA",   "AA=",    "AB==", "AAB=", "AA-A",
                         "AA_A", "AA A", "AA\\nA", "====", "=AAA", "AA==AAAA"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char body[512];
        snprintf(
            body, sizeof(body),
            "{\"filename\":\"x\",\"content_type\":\"x\",\"encoding\":\"base64\",\"data\":\"%s\"}",
            bad[i]);
        expect("/api/upload", body, "application/json", 400);
    }
    expect("/api/upload", "{}", "application/json", 400);
    if (geteuid() != 0) {
        assert(chmod("uploads_storage", 0500) == 0);
        expect("/api/upload", good, "application/json", 500);
        assert(chmod("uploads_storage", 0700) == 0);
    } else {
        puts("POST: permission-denied case skipped for root (directory-missing case still tested)");
    }
    DIR *directory = opendir("uploads_storage");
    assert(directory);
    size_t files = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (entry->d_name[0] != '.') {
            files++;
            char name[256];
            snprintf(name, sizeof(name), "uploads_storage/%s", entry->d_name);
            assert(!unlink(name));
        }
    closedir(directory);
    assert(files == 4); /* Exactly two successful file pairs. */
    assert(!rmdir("uploads_storage") && !fchdir(original));
    close(original);
    assert(!rmdir(temp));
    puts("POST: routes, JSON/form media types, strict Base64 and readonly file pairs passed");
}
