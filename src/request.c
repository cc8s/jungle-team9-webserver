#include "request.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

static enum parse_result fail(struct request *req, int status)
{
    req->status = status;
    return PARSE_ERROR;
}

/* RFC 9110 token 문자: 메서드와 헤더 이름에 쓸 수 있는 글자 */
static int is_tchar(unsigned char c)
{
    return isalnum(c) || (c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static int all_tchar(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!is_tchar((unsigned char)p[i]))
            return 0;
    return n > 0;
}

/* 헤더 이름 [p, p+n) 이 name 과 대소문자 무시하고 같은가 */
static int name_is(const char *p, size_t n, const char *name)
{
    return strlen(name) == n && strncasecmp(p, name, n) == 0;
}

/* "METHOD SP URI SP HTTP/1.x" 한 줄 [p, end) */
static int parse_request_line(const char *p, const char *end, struct request *req)
{
    const char *sp1 = memchr(p, ' ', end - p);
    if (sp1 == NULL)
        return 400;
    const char *sp2 = memchr(sp1 + 1, ' ', end - (sp1 + 1));
    if (sp2 == NULL)
        return 400;

    size_t mlen = sp1 - p, ulen = sp2 - (sp1 + 1), vlen = end - (sp2 + 1);
    if (mlen > REQ_MAX_METHOD)
        return 501;
    if (!all_tchar(p, mlen))
        return 400;
    if (ulen > REQ_MAX_URI)
        return 414;
    if (ulen == 0 || sp1[1] != '/') /* origin-form 만 받는다 (리버스 프록시라 절대 URI 불필요) */
        return 400;
    for (const char *u = sp1 + 1; u < sp2; u++)
        if ((unsigned char)*u <= 0x20 || *u == 0x7f) /* 제어 문자·공백 금지 */
            return 400;

    const char *v = sp2 + 1;
    if (vlen != 8 || memcmp(v, "HTTP/", 5) != 0 || !isdigit((unsigned char)v[5]) || v[6] != '.' ||
        !isdigit((unsigned char)v[7]))
        return 400;
    if (v[5] != '1' || (v[7] != '0' && v[7] != '1')) /* 모양은 맞지만 1.0·1.1 이 아님 */
        return 505;

    memcpy(req->method, p, mlen);
    memcpy(req->uri, sp1 + 1, ulen);
    req->minor = v[7] - '0';
    return 0;
}

enum parse_result parse_request(const char *buf, size_t len, struct request *req)
{
    memset(req, 0, sizeof(*req));

    /* 1. 헤더 끝(빈 줄)을 찾는다. 상한 안에서 못 찾으면 덜 온 것, 상한을 넘으면 오류 */
    const char *hend = memmem(buf, len, "\r\n\r\n", 4);
    size_t scan = hend ? (size_t)(hend - buf) + 4 : len;
    if (scan > REQ_MAX_HEADER)
        return fail(req, memmem(buf, REQ_MAX_HEADER, "\r\n", 2) ? 400 : 414);
    if (hend == NULL)
        return PARSE_INCOMPLETE;

    /* 2. 요청 라인 */
    const char *rl_end = memmem(buf, hend + 2 - buf, "\r\n", 2); /* 반드시 찾음 (hend 자리) */
    int st = parse_request_line(buf, rl_end, req);
    if (st != 0)
        return fail(req, st);

    /* 3. 헤더 줄들: [rl_end+2, hend+2) 안의 "이름: 값\r\n" */
    int seen_host = 0, seen_cl = 0, conn_close = 0, conn_keep = 0;
    for (const char *p = rl_end + 2; p < hend + 2;)
    {
        const char *e = memmem(p, hend + 2 - p, "\r\n", 2);
        if (*p == ' ' || *p == '\t') /* 옛 줄 접기(obs-fold) 거절 */
            return fail(req, 400);
        if (memchr(p, '\r', e - p) || memchr(p, '\0', e - p)) /* 줄 안의 CR·NUL 은 요청 밀반입 수단 */
            return fail(req, 400);
        const char *colon = memchr(p, ':', e - p);
        if (colon == NULL || !all_tchar(p, colon - p)) /* "Host : x" 처럼 이름 뒤 공백도 여기서 걸림 */
            return fail(req, 400);

        const char *v = colon + 1, *ve = e; /* 값 앞뒤 공백·탭 제거 */
        while (v < ve && (*v == ' ' || *v == '\t'))
            v++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
            ve--;
        size_t nlen = colon - p, vlen = ve - v;

        if (name_is(p, nlen, "Host"))
        {
            if (seen_host++ || vlen > REQ_MAX_HOST)
                return fail(req, 400);
            memcpy(req->host, v, vlen);
        }
        else if (name_is(p, nlen, "Content-Length"))
        {
            if (seen_cl++ || vlen == 0) /* 중복은 같은 값이어도 거절 (밀반입 방지) */
                return fail(req, 400);
            size_t n = 0;
            for (const char *d = v; d < ve; d++)
            {
                if (!isdigit((unsigned char)*d))
                    return fail(req, 400);
                n = n * 10 + (*d - '0');
                if (n > REQ_MAX_BODY) /* 넘는 순간 멈춰서 오버플로도 막는다 */
                    return fail(req, 413);
            }
            req->content_length = n;
        }
        else if (name_is(p, nlen, "Transfer-Encoding"))
            return fail(req, 501); /* chunked 미지원 (D5) */
        else if (name_is(p, nlen, "Connection"))
        {
            /* "keep-alive, Upgrade" 처럼 쉼표로 여러 개 올 수 있다 */
            for (const char *t = v; t < ve;)
            {
                while (t < ve && (*t == ' ' || *t == ','))
                    t++;
                const char *te = t;
                while (te < ve && *te != ',' && *te != ' ')
                    te++;
                conn_close |= name_is(t, te - t, "close");
                conn_keep |= name_is(t, te - t, "keep-alive");
                t = te;
            }
        }
        p = e + 2;
    }

    if (req->minor == 1 && !seen_host) /* HTTP/1.1 은 Host 필수 */
        return fail(req, 400);
    /* 1.1 은 기본 유지, 1.0 은 기본 종료. close 가 있으면 무조건 종료 */
    req->keep_alive = !conn_close && (req->minor == 1 || conn_keep);

    /* 4. 본문: Content-Length 만큼 다 왔는지 */
    req->header_len = hend + 4 - buf;
    if (len - req->header_len < req->content_length)
        return PARSE_INCOMPLETE;
    req->body = buf + req->header_len;
    req->total_len = req->header_len + req->content_length;
    return PARSE_OK;
}
