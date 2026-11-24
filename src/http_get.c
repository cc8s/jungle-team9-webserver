#include "http_get.h"
#include "http_response.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *get_mime_type(const char *path)
{
    static const struct { const char *ext, *mime; } types[] = {
        {".html", "text/html"}, {".css", "text/css"}, {".js", "text/javascript"},
        {".json", "application/json"}, {".png", "image/png"},
        {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}
    };
    const char *ext = strrchr(path, '.');
    for (size_t i = 0; ext && i < sizeof(types)/sizeof(types[0]); i++)
        if (!strcasecmp(ext, types[i].ext)) return types[i].mime;
    return "application/octet-stream";
}

static int file_status(int error)
{
    if (error == ENOENT) return 404;
    if (error == EACCES || error == EPERM || error == ELOOP || error == ENOTDIR) return 403;
    return 500;
}

/* Walk from www one component at a time; never follow a symlink. */
static int open_static(char *path, int *status)
{
    int directory = open("www", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0) { *status = 500; return -1; }
    char *part = path;
    while (*part == '/') part++;
    while (*part) {
        char *slash = strchr(part, '/');
        if (slash) *slash = 0;
        struct stat st;
        if (fstatat(directory, part, &st, AT_SYMLINK_NOFOLLOW) < 0) {
            *status = file_status(errno); close(directory); return -1;
        }
        if (S_ISLNK(st.st_mode)) { *status = 403; close(directory); return -1; }
        int next = openat(directory, part, O_RDONLY | O_NOFOLLOW | O_NONBLOCK |
                          (slash ? O_DIRECTORY : 0));
        int saved = errno;
        close(directory);
        if (next < 0) { *status = file_status(saved); return -1; }
        directory = next;
        if (!slash) return directory;
        part = slash + 1;
        while (*part == '/') part++;
    }
    *status = 403;
    close(directory);
    return -1;
}

int get_prepare(char *path, get_response *response)
{
    *response = (get_response){.file = -1};
    const char *mime = get_mime_type(path);
    int status = 0;
    int file = open_static(path, &status);
    if (file < 0)
        return status;
    struct stat st;
    if (fstat(file, &st) < 0)
        status = 500;
    else if (!S_ISREG(st.st_mode))
        status = 403;
    else if (st.st_size < 0)
        status = 500;
    if (status) {
        close(file);
        return status;
    }
    *response = (get_response){.file = file, .mime = mime, .length = (uintmax_t)st.st_size};
    return 200;
}

int get_send_body(int fd, const get_response *response, int64_t *deadline)
{
    uintmax_t left = response->length;
    unsigned char chunk[16384];
    while (left > 0) {
        size_t count = left > sizeof(chunk) ? sizeof(chunk) : (size_t)left;
        ssize_t n = read(response->file, chunk, count);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || http_send_all(fd, chunk, (size_t)n, deadline) < 0)
            return -1;
        left -= (size_t)n;
    }
    return 0;
}
