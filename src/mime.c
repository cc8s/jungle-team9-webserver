#include "mime.h"

#include <string.h>
#include <strings.h>

static const struct
{
    const char *ext;
    const char *type;
} table[] = {
    /* 글자 파일은 charset 을 붙여 한글이 깨지지 않게 */
    {"html", "text/html; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"js", "text/javascript; charset=utf-8"},
    {"mjs", "text/javascript; charset=utf-8"},
    {"json", "application/json"},
    {"txt", "text/plain; charset=utf-8"},
    {"svg", "image/svg+xml"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"webp", "image/webp"},
    {"ico", "image/x-icon"},
    {"pdf", "application/pdf"},
};

const char *mime_type(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.'); /* 마지막 점: "a.tar.gz" 는 "gz" */

    if (dot != NULL && dot != base) /* ".bashrc" 처럼 점으로 시작하는 이름은 확장자 없음 */
        for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
            if (strcasecmp(dot + 1, table[i].ext) == 0)
                return table[i].type;
    return "application/octet-stream";
}
