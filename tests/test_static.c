#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "check.h"
#include "static.h"

static char root[PATH_MAX];
static char resp[256 * 1024];

/* method uri 를 serve_static 에 넘기고, 받은 응답 전체를 resp 에 담는다. 반환: serve_static 결과 */
static int run(const char *method, const char *uri)
{
    struct request req;
    int sv[2], status;
    size_t got = 0;
    ssize_t k;

    memset(&req, 0, sizeof(req));
    strcpy(req.method, method);
    strcpy(req.uri, uri);
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    status = serve_static(sv[0], root, &req);
    close(sv[0]); /* 보내는 쪽을 닫아야 아래 read 가 끝(EOF)을 만난다 */
    while ((k = read(sv[1], resp + got, sizeof(resp) - 1 - got)) > 0)
        got += (size_t)k;
    resp[got] = '\0';
    close(sv[1]);
    return status;
}

static const char *body_of(void)
{
    const char *p = strstr(resp, "\r\n\r\n");
    return p ? p + 4 : "";
}

int main(void)
{
    /* 메서드 규칙 (D9) */
    CHECK(static_method_status("GET") == 0);
    CHECK(static_method_status("HEAD") == 0);
    CHECK(static_method_status("POST") == 405);
    CHECK(static_method_status("DELETE") == 405);
    CHECK(static_method_status("get") == 501); /* 메서드는 대소문자 구분 → 모르는 메서드 */
    CHECK(static_method_status("FOO") == 501);

    /* 임시 루트: index.html, style.css, 100KB big.bin (64KB 조각 두 번에 걸치게) */
    char base[] = "/tmp/webserver-static-XXXXXX";
    CHECK(mkdtemp(base) != NULL);
    CHECK(realpath(base, root) != NULL);
    char p[PATH_MAX + 32];
    FILE *f;
    snprintf(p, sizeof(p), "%s/index.html", root);
    f = fopen(p, "w"), fputs("<h1>hi</h1>", f), fclose(f);
    snprintf(p, sizeof(p), "%s/style.css", root);
    f = fopen(p, "w"), fputs("h1{}", f), fclose(f);
    snprintf(p, sizeof(p), "%s/big.bin", root);
    f = fopen(p, "w");
    for (int i = 0; i < 100 * 1024; i++)
        fputc(i % 251, f);
    fclose(f);

    /* GET → 200, 타입·길이·본문 */
    CHECK(run("GET", "/") == 200);
    CHECK(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(strstr(resp, "\r\nContent-Type: text/html; charset=utf-8\r\n") != NULL);
    CHECK(strstr(resp, "\r\nContent-Length: 11\r\n") != NULL);
    CHECK(strcmp(body_of(), "<h1>hi</h1>") == 0);

    CHECK(run("GET", "/style.css") == 200);
    CHECK(strstr(resp, "\r\nContent-Type: text/css; charset=utf-8\r\n") != NULL);

    /* 큰 파일: 길이·내용이 끝까지 맞나 */
    CHECK(run("GET", "/big.bin") == 200);
    CHECK(strstr(resp, "\r\nContent-Length: 102400\r\n") != NULL);
    const char *b = strstr(resp, "\r\n\r\n") + 4;
    int ok = 1;
    for (int i = 0; i < 100 * 1024; i++)
        ok &= b[i] == (char)(i % 251);
    CHECK(ok);

    /* HEAD → 헤더만, 길이는 GET 과 같게 */
    CHECK(run("HEAD", "/") == 200);
    CHECK(strstr(resp, "\r\nContent-Length: 11\r\n") != NULL);
    CHECK(body_of()[0] == '\0');

    /* 오류들 */
    CHECK(run("GET", "/nope.html") == 404 && strncmp(resp, "HTTP/1.1 404 ", 13) == 0);
    CHECK(run("GET", "/../etc/passwd") == 403 && strncmp(resp, "HTTP/1.1 403 ", 13) == 0);
    CHECK(run("POST", "/") == 405 && strstr(resp, "\r\nAllow: GET, HEAD\r\n") != NULL);
    CHECK(run("FOO", "/") == 501);
    CHECK(run("HEAD", "/nope.html") == 404 && body_of()[0] == '\0'); /* 오류도 HEAD 면 본문 없음 */
    CHECK(run("GET", "/<script>") == 404 && strstr(resp, "<script>") == NULL); /* D7 */

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    CHECK(system(cmd) == 0);

    CHECK_DONE();
}
