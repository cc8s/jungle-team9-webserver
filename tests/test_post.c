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
int main(void)
{
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
    expect("/api/upload", "", NULL, 200);
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
