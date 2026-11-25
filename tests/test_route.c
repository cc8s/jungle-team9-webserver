#include <string.h>

#include "check.h"
#include "route.h"

static struct route r;
static const char *err;

#define OK(spec) (route_parse((spec), &r, &err) == 0)
#define BAD(spec) (route_parse((spec), &r, &err) == -1)

int main(void)
{
    /* ---- 해석 ---- */
    CHECK(OK("/api=127.0.0.1:8001"));
    CHECK(strcmp(r.prefix, "/api") == 0 && r.nbe == 1);
    CHECK(strcmp(r.be[0].host, "127.0.0.1") == 0 && strcmp(r.be[0].port, "8001") == 0);

    CHECK(OK("/api/v1=localhost:80,backend-2.local:9000"));
    CHECK(strcmp(r.prefix, "/api/v1") == 0 && r.nbe == 2);
    CHECK(strcmp(r.be[1].host, "backend-2.local") == 0 && strcmp(r.be[1].port, "9000") == 0);

    CHECK(BAD("api=h:80"));            /* / 로 시작 안 함 */
    CHECK(BAD("/api"));                /* = 없음 */
    CHECK(BAD("/=h:80"));              /* "/" 는 정적 파일을 전부 가림 */
    CHECK(BAD("/api/=h:80"));          /* 끝의 / */
    CHECK(BAD("/api="));               /* 백엔드 없음 */
    CHECK(BAD("/api=h"));              /* 포트 없음 */
    CHECK(BAD("/api=h:"));
    CHECK(BAD("/api=:80"));            /* 호스트 없음 */
    CHECK(BAD("/api=h:0"));
    CHECK(BAD("/api=h:65536"));
    CHECK(BAD("/api=h:80x"));
    CHECK(BAD("/api=h:80,"));          /* 쉼표로 끝남 */
    CHECK(BAD("/api=h:80,,g:81"));     /* 빈 항목 */
    CHECK(BAD("/api=h@x:80"));         /* 호스트에 못 쓰는 글자 */
    CHECK(BAD("/api=[::1]:80"));       /* IPv6 리터럴은 지원 안 함 */
    CHECK(BAD("/api=a:1,a:2,a:3,a:4,a:5,a:6,a:7,a:8,a:9")); /* 9개 */
    CHECK(err != NULL);

    /* ---- 경로 조각 단위 매칭 (D4) ---- */
    CHECK(OK("/api=h:80"));
    CHECK(route_matches(&r, "/api"));
    CHECK(route_matches(&r, "/api/"));
    CHECK(route_matches(&r, "/api/users"));
    CHECK(route_matches(&r, "/api?x=1"));
    CHECK(route_matches(&r, "/api/users?id=3"));
    CHECK(!route_matches(&r, "/apix"));        /* 조각이 아님 */
    CHECK(!route_matches(&r, "/apix/hello"));
    CHECK(!route_matches(&r, "/API/users"));   /* 대소문자 구분 */
    CHECK(!route_matches(&r, "/ap"));
    CHECK(!route_matches(&r, "/"));
    CHECK(!route_matches(&r, "/x/api"));

    /* ---- 여러 규칙 중 고르기: 가장 긴 접두어 ---- */
    struct route rs[3];
    CHECK(route_parse("/api=a:1", &rs[0], &err) == 0);
    CHECK(route_parse("/api/v2=b:2", &rs[1], &err) == 0);
    CHECK(route_parse("/img=c:3", &rs[2], &err) == 0);
    CHECK(route_find(rs, 3, "/api/users") == &rs[0]);
    CHECK(route_find(rs, 3, "/api/v2/users") == &rs[1]); /* 더 긴 쪽 */
    CHECK(route_find(rs, 3, "/api/v2") == &rs[1]);
    CHECK(route_find(rs, 3, "/api/v22") == &rs[0]);      /* v2 는 조각이 아니라 /api 로 */
    CHECK(route_find(rs, 3, "/img/a.png") == &rs[2]);
    CHECK(route_find(rs, 3, "/index.html") == NULL);     /* 정적 파일로 */
    CHECK(route_find(rs, 0, "/api") == NULL);            /* 규칙 없음 */

    /* ---- Round-Robin ---- */
    CHECK(route_parse("/x=a:1,b:2,c:3", &r, &err) == 0);
    int seq[7];
    for (int i = 0; i < 7; i++)
        seq[i] = route_next_start(&r);
    CHECK(seq[0] == 0 && seq[1] == 1 && seq[2] == 2 && seq[3] == 0 && seq[4] == 1 && seq[5] == 2 && seq[6] == 0);
    CHECK(route_parse("/y=a:1", &r, &err) == 0);
    CHECK(route_next_start(&r) == 0 && route_next_start(&r) == 0); /* 백엔드 하나면 항상 0 */

    CHECK_DONE();
}
