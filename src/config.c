#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* s 가 lo~hi 범위의 10진수 정수 문자열이면 1. "8080abc", "", "-1" 은 0 */
static int is_int_in_range(const char *s, long lo, long hi, long *out)
{
    char *end;
    long v;

    if (s == NULL || *s == '\0')
        return 0;
    v = strtol(s, &end, 10);
    if (*end != '\0' || v < lo || v > hi)
        return 0;
    if (out)
        *out = v;
    return 1;
}

int parse_args(int argc, char **argv, struct config *cfg, const char **err)
{
    int c;
    long v;

    memset(cfg, 0, sizeof(*cfg));
    cfg->timeout_sec = DEFAULT_TIMEOUT_SEC;

    optind = 0; /* glibc: 0 이면 getopt 내부 상태를 처음부터 다시 (테스트에서 여러 번 부르기 위해) */
    opterr = 0; /* getopt 가 직접 화면에 오류를 찍지 않게. 오류는 *err 로 돌려준다 */

    while ((c = getopt(argc, argv, "p:r:R:t:")) != -1) {
        switch (c) {
        case 'p':
            if (!is_int_in_range(optarg, 1, 65535, NULL)) {
                *err = "port must be 1-65535";
                return -1;
            }
            cfg->port = optarg;
            break;
        case 'r':
            cfg->root = optarg;
            break;
        case 'R':
            if (cfg->nroutes == MAX_ROUTES) {
                *err = "too many -R routes";
                return -1;
            }
            cfg->routes[cfg->nroutes++] = optarg;
            break;
        case 't':
            if (!is_int_in_range(optarg, 1, 3600, &v)) {
                *err = "timeout must be 1-3600 seconds";
                return -1;
            }
            cfg->timeout_sec = (int)v;
            break;
        default: /* 모르는 옵션이거나 값이 빠진 옵션 ('?') */
            *err = "unknown option or missing value";
            return -1;
        }
    }

    if (optind < argc) {
        *err = "unexpected extra argument";
        return -1;
    }
    if (cfg->port == NULL || cfg->root == NULL) {
        *err = "-p <port> and -r <root> are required";
        return -1;
    }
    return 0;
}
