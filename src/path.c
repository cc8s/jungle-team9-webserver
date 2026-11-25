#include "path.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int url_decode_path(const char *uri, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = uri; *p != '\0' && *p != '?'; p++)
    {
        char c = *p;
        if (c == '%')
        {
            int hi = hexval(p[1]), lo = hi < 0 ? -1 : hexval(p[2]); /* p[1] 이 '\0' 이면 p[2] 를 안 읽게 */
            if (hi < 0 || lo < 0)
                return -1;
            c = (char)(hi * 16 + lo);
            if (c == '\0') /* "%00": C 문자열이 여기서 잘려 검사와 실제 열기가 달라진다 */
                return -1;
            p += 2;
        }
        if (o + 1 >= cap)
            return -1;
        out[o++] = c;
    }
    out[o] = '\0';
    return 0;
}

int has_dotdot_segment(const char *path)
{
    for (const char *p = path; (p = strstr(p, "..")) != NULL; p += 2)
    {
        int starts = p == path || p[-1] == '/';
        int ends = p[2] == '\0' || p[2] == '/';
        if (starts && ends)
            return 1;
    }
    return 0;
}

/* path 가 root 와 같거나 root 바로 아래인가 (조각 단위: "/srv/www2" 는 "/srv/www" 안이 아님) */
static int is_inside(const char *root, const char *path)
{
    size_t n = strlen(root);
    if (n == 1) /* 루트가 "/" 면 전부 안 */
        return 1;
    return strncmp(path, root, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

/* realpath 실패를 상태 코드로 */
static int errno_status(void)
{
    if (errno == ENOENT || errno == ENOTDIR)
        return 404;
    if (errno == EACCES)
        return 403;
    return 500;
}

int resolve_path(const char *root_real, const char *uri, char *out, size_t cap)
{
    char decoded[PATH_MAX], joined[PATH_MAX], real[PATH_MAX];
    struct stat st;

    /* 1. 디코딩은 딱 한 번 → 2. 디코딩한 결과에서 ".." 검사 (두 번 디코딩하면 "%252e" 가 빠져나감) */
    if (url_decode_path(uri, decoded, sizeof(decoded)) < 0)
        return 400;
    if (has_dotdot_segment(decoded))
        return 403;

    /* 3. 루트에 붙인다 ("/" 나 "/docs/" 같은 디렉터리는 5 에서 index.html 로) */
    int n = snprintf(joined, sizeof(joined), "%s%s", root_real, decoded);
    if (n < 0 || (size_t)n >= sizeof(joined))
        return 404; /* 그렇게 긴 경로의 파일은 없다 */

    /* 4. 심볼릭 링크까지 다 풀어 실제 위치를 구하고, 그게 루트 안인지 확인 */
    if (realpath(joined, real) == NULL)
        return errno_status();
    if (!is_inside(root_real, real))
        return 403;

    /* 5. 디렉터리면 그 안의 index.html 로 한 번 더 (목록은 보여 주지 않는다) */
    if (stat(real, &st) < 0)
        return errno_status();
    if (S_ISDIR(st.st_mode))
    {
        n = snprintf(joined, sizeof(joined), "%s/index.html", real);
        if (n < 0 || (size_t)n >= sizeof(joined))
            return 404;
        if (realpath(joined, real) == NULL)
        {
            int e = errno_status();
            return e == 404 ? 403 : e; /* index 없는 디렉터리 → 403 (nginx 와 같음, 목록 비공개) */
        }
        if (!is_inside(root_real, real) || stat(real, &st) < 0)
            return 403;
    }
    if (!S_ISREG(st.st_mode)) /* 장치 파일·FIFO 등은 내주지 않는다 */
        return 403;

    if (strlen(real) + 1 > cap)
        return 500;
    strcpy(out, real);
    return 0;
}
