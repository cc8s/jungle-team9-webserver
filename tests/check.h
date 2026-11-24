#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>

/* 아주 작은 테스트 도우미. 실패한 줄을 찍고 개수를 센다. 마지막에 CHECK_DONE() 으로 종료 코드를 정한다. */
static int check_failed;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            check_failed++;                                             \
        }                                                               \
    } while (0)

#define CHECK_DONE()                                                    \
    do {                                                                \
        printf(check_failed ? "  %d failed\n" : "  ok\n", check_failed);\
        return check_failed ? 1 : 0;                                    \
    } while (0)

#endif
