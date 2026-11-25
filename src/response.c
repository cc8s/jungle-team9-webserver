#include "response.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

const char *status_text(int status)
{
    switch (status)
    {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return NULL;
    }
}

/* buf[*off..cap) 에 printf 처럼 이어 쓴다. 자리가 모자라면 -1 */
static int append(char *buf, size_t cap, size_t *off, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *off) /* vsnprintf 는 잘렸을 때 "썼어야 할 길이" 를 돌려준다 */
        return -1;
    *off += n;
    return 0;
}

int build_header(char *buf, size_t cap, int status, const char *content_type, size_t content_length,
                 int keep_alive, const char *extra)
{
    const char *reason = status_text(status);
    size_t off = 0;
    char date[64];
    time_t now = time(NULL);
    struct tm tm;

    if (reason == NULL || cap == 0)
        return -1;
    /* RFC 9110: 시계가 있는 서버는 Date 를 보낸다. 형식은 항상 GMT */
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", gmtime_r(&now, &tm));

    if (append(buf, cap, &off, "HTTP/1.1 %d %s\r\n", status, reason) < 0 ||
        append(buf, cap, &off, "Date: %s\r\n", date) < 0 ||
        (content_type && append(buf, cap, &off, "Content-Type: %s\r\n", content_type) < 0) ||
        append(buf, cap, &off, "Content-Length: %zu\r\n", content_length) < 0 ||
        append(buf, cap, &off, "Connection: %s\r\n", keep_alive ? "keep-alive" : "close") < 0 ||
        append(buf, cap, &off, "X-Content-Type-Options: nosniff\r\n") < 0 || /* 브라우저가 타입을 추측하지 않게 */
        (extra && append(buf, cap, &off, "%s", extra) < 0) ||
        append(buf, cap, &off, "\r\n") < 0)
        return -1;
    return (int)off;
}

int build_error(char *buf, size_t cap, int status, int head_only)
{
    const char *reason = status_text(status);
    char body[256];

    if (reason == NULL)
        return -1;
    int blen = snprintf(body, sizeof(body), "<!doctype html><title>%d %s</title><h1>%d %s</h1>\n", status,
                        reason, status, reason);
    /* 405 는 무엇이 허용되는지 Allow 로 알려야 한다 (RFC 9110). 정적 경로 기준 (D9) */
    const char *extra = status == 405 ? "Allow: GET, HEAD\r\n" : NULL;

    /* 오류 뒤에는 연결 상태를 믿을 수 없으니 항상 닫는다 (D5) */
    int hlen = build_header(buf, cap, status, "text/html; charset=utf-8", (size_t)blen, 0, extra);
    if (hlen < 0)
        return -1;
    if (head_only) /* HEAD: Content-Length 는 GET 과 같게 두고 본문만 뺀다 */
        return hlen;
    if ((size_t)hlen + blen > cap)
        return -1;
    memcpy(buf + hlen, body, blen);
    return hlen + blen;
}

int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0)
    {
        ssize_t w = write(fd, p, n);
        if (w < 0)
        {
            if (errno == EINTR) /* 시그널에 끊긴 것뿐이면 다시 */
                continue;
            return -1;
        }
        /* 소켓 버퍼가 덜 비어 있으면 일부만 써질 수 있다 → 남은 만큼 다시 */
        p += w;
        n -= (size_t)w;
    }
    return 0;
}
