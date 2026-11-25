#include <string.h>

#include "check.h"
#include "request.h"

static struct request req;
static char big[REQ_MAX_HEADER + 4096];

/* 문자열 상수 하나를 통째로 파싱 */
#define P(s) parse_request((s), strlen(s), &req)
/* 오류이고 상태 코드가 code 인가 */
#define ERR(s, code) (P(s) == PARSE_ERROR && req.status == (code))

int main(void)
{
    /* ---- 정상 ---- */
    const char *get = "GET /index.html HTTP/1.1\r\nHost: localhost\r\nUser-Agent: t\r\n\r\n";
    CHECK(P(get) == PARSE_OK);
    CHECK(strcmp(req.method, "GET") == 0);
    CHECK(strcmp(req.uri, "/index.html") == 0);
    CHECK(req.minor == 1);
    CHECK(strcmp(req.host, "localhost") == 0);
    CHECK(req.keep_alive == 1);
    CHECK(req.content_length == 0);
    CHECK(req.header_len == strlen(get) && req.total_len == strlen(get));

    /* 나뉘어 들어옴: 모든 앞부분은 "덜 옴", 전체가 와야 OK */
    int all_incomplete = 1;
    for (size_t k = 0; k < strlen(get); k++)
        all_incomplete &= parse_request(get, k, &req) == PARSE_INCOMPLETE;
    CHECK(all_incomplete);

    /* POST 본문 */
    const char *post = "POST /api/x HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello";
    CHECK(P(post) == PARSE_OK);
    CHECK(req.content_length == 5 && memcmp(req.body, "hello", 5) == 0);
    CHECK(parse_request(post, strlen(post) - 1, &req) == PARSE_INCOMPLETE); /* 본문 1바이트 모자람 */

    /* 요청 두 개가 붙어 옴 (파이프라이닝): 첫 요청 길이만 total_len */
    char two[256];
    strcpy(two, post);
    strcat(two, get);
    CHECK(P(two) == PARSE_OK && req.total_len == strlen(post));

    /* 헤더 이름 대소문자 무시, 값 앞뒤 공백 제거 */
    CHECK(P("GET / HTTP/1.1\r\nhOsT:   example.com  \r\n\r\n") == PARSE_OK);
    CHECK(strcmp(req.host, "example.com") == 0);

    /* 모르는 메서드도 파서는 통과 (501/405 는 처리기가 정한다, D9) */
    CHECK(P("FOO / HTTP/1.1\r\nHost: a\r\n\r\n") == PARSE_OK);

    /* ---- Connection ---- */
    CHECK(P("GET / HTTP/1.0\r\n\r\n") == PARSE_OK && req.keep_alive == 0); /* 1.0 은 Host 없어도 됨 */
    CHECK(P("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n") == PARSE_OK && req.keep_alive == 1);
    CHECK(P("GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n") == PARSE_OK && req.keep_alive == 0);
    CHECK(P("GET / HTTP/1.1\r\nHost: a\r\nConnection: Keep-Alive, close\r\n\r\n") == PARSE_OK &&
          req.keep_alive == 0);

    /* ---- 요청 라인 오류 ---- */
    CHECK(ERR("GET /\r\n\r\n", 400));                       /* 버전 없음 */
    CHECK(ERR("GET index.html HTTP/1.1\r\nHost: a\r\n\r\n", 400)); /* / 로 시작 안 함 */
    CHECK(ERR("GE(T / HTTP/1.1\r\nHost: a\r\n\r\n", 400));  /* 메서드에 못 쓰는 글자 */
    CHECK(ERR("ABCDEFGHIJKLMNOPQ / HTTP/1.1\r\nHost: a\r\n\r\n", 501)); /* 17자 메서드 */
    CHECK(ERR("HTTX/1.1 / HTTP/1.1x\r\n\r\n", 400));
    CHECK(ERR("GET / HTTX/1.1\r\nHost: a\r\n\r\n", 400));
    CHECK(ERR("GET / HTTP/2.0\r\nHost: a\r\n\r\n", 505));
    CHECK(ERR("GET / HTTP/1.2\r\nHost: a\r\n\r\n", 505));

    /* ---- 헤더 오류 ---- */
    CHECK(ERR("GET / HTTP/1.1\r\n\r\n", 400));                         /* 1.1 인데 Host 없음 */
    CHECK(ERR("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400));   /* Host 중복 */
    CHECK(ERR("GET / HTTP/1.1\r\nHost : a\r\n\r\n", 400));             /* 이름 뒤 공백 */
    CHECK(ERR("GET / HTTP/1.1\r\nHost: a\r\nX-A: 1\r\n  folded\r\n\r\n", 400)); /* obs-fold */
    CHECK(ERR("GET / HTTP/1.1\r\nHost: a\r\nnocolon\r\n\r\n", 400));
    CHECK(ERR("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: abc\r\n\r\n", 400));
    CHECK(ERR("POST / HTTP/1.1\r\nHost: a\r\nContent-Length:\r\n\r\n", 400));
    CHECK(ERR("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx", 400));
    CHECK(ERR("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 2000000\r\n\r\n", 413));
    CHECK(ERR("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999999999999999999\r\n\r\n", 413));
    CHECK(ERR("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", 501));

    /* 줄 안의 NUL (strlen 으로는 못 만드니 길이를 직접 준다) */
    const char nul[] = "GET / HTTP/1.1\r\nHost: a\0b\r\n\r\n";
    CHECK(parse_request(nul, sizeof(nul) - 1, &req) == PARSE_ERROR && req.status == 400);

    /* ---- 길이 상한 ---- */
    /* URI 2048 자는 통과, 2049 자는 414 */
    size_t n;
    for (int extra = 0; extra <= 1; extra++)
    {
        n = 0;
        memcpy(big, "GET /", 5), n = 5;
        memset(big + n, 'a', REQ_MAX_URI - 1 + extra), n += REQ_MAX_URI - 1 + extra;
        memcpy(big + n, " HTTP/1.1\r\nHost: a\r\n\r\n", 23), n += 23;
        enum parse_result r = parse_request(big, n, &req);
        CHECK(extra == 0 ? r == PARSE_OK : (r == PARSE_ERROR && req.status == 414));
    }

    /* 요청 라인이 상한 안에서 안 끝남 → 414 */
    memcpy(big, "GET /", 5);
    memset(big + 5, 'a', sizeof(big) - 5);
    CHECK(parse_request(big, sizeof(big), &req) == PARSE_ERROR && req.status == 414);

    /* 요청 라인은 끝났지만 헤더가 상한 안에서 안 끝남 → 400 */
    memcpy(big, "GET / HTTP/1.1\r\nX: ", 19);
    memset(big + 19, 'a', sizeof(big) - 19);
    CHECK(parse_request(big, sizeof(big), &req) == PARSE_ERROR && req.status == 400);

    /* 상한 바로 아래에서 아직 안 끝났으면 "덜 옴" */
    CHECK(parse_request(big, REQ_MAX_HEADER, &req) == PARSE_INCOMPLETE);

    CHECK_DONE();
}
