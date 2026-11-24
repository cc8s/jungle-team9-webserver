#include <string.h>

#include "check.h"
#include "config.h"

/* parse_args 는 argv 를 고치지 않지만 타입이 char ** 라서, 문자열 상수를 배열에 담아 넘긴다 */
#define ARGV(...) ((char *[]){"webserver", __VA_ARGS__, NULL})
#define ARGC(...) ((int)(sizeof((char *[]){"webserver", __VA_ARGS__}) / sizeof(char *)))
#define PARSE(...) parse_args(ARGC(__VA_ARGS__), ARGV(__VA_ARGS__), &cfg, &err)

int main(void)
{
    struct config cfg;
    const char *err;

    /* 정상: 필수 두 개만 → 기본 타임아웃 */
    CHECK(PARSE("-p", "8080", "-r", "www") == 0);
    CHECK(strcmp(cfg.port, "8080") == 0);
    CHECK(strcmp(cfg.root, "www") == 0);
    CHECK(cfg.nroutes == 0);
    CHECK(cfg.timeout_sec == DEFAULT_TIMEOUT_SEC);

    /* 정상: -R 여러 번, -t */
    CHECK(PARSE("-p", "80", "-r", "www", "-R", "/api=127.0.0.1:8001", "-R", "/b=127.0.0.1:9000",
                "-t", "10") == 0);
    CHECK(cfg.nroutes == 2);
    CHECK(strcmp(cfg.routes[1], "/b=127.0.0.1:9000") == 0);
    CHECK(cfg.timeout_sec == 10);

    /* 필수 값 빠짐 */
    CHECK(PARSE("-p", "8080") == -1);
    CHECK(PARSE("-r", "www") == -1);

    /* 포트 범위·형식 */
    CHECK(PARSE("-p", "0", "-r", "www") == -1);
    CHECK(PARSE("-p", "65536", "-r", "www") == -1);
    CHECK(PARSE("-p", "80abc", "-r", "www") == -1);
    CHECK(PARSE("-p", "", "-r", "www") == -1);

    /* 타임아웃 형식 */
    CHECK(PARSE("-p", "8080", "-r", "www", "-t", "0") == -1);
    CHECK(PARSE("-p", "8080", "-r", "www", "-t", "five") == -1);

    /* 모르는 옵션, 값 없는 옵션, 남는 인자 */
    CHECK(PARSE("-p", "8080", "-r", "www", "-x") == -1);
    CHECK(PARSE("-p", "8080", "-r") == -1);
    CHECK(PARSE("-p", "8080", "-r", "www", "extra") == -1);

    /* -R 상한 */
    CHECK(PARSE("-p", "8080", "-r", "www", "-R", "a", "-R", "a", "-R", "a", "-R", "a", "-R", "a",
                "-R", "a", "-R", "a", "-R", "a", "-R", "a") == -1);

    CHECK_DONE();
}
