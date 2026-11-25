#ifndef ROUTE_H
#define ROUTE_H

#include <sys/socket.h>

#define MAX_BACKENDS 8
#define MAX_PREFIX 128
#define MAX_HOST 255

struct backend
{
    char host[MAX_HOST + 1];
    char port[8];
    struct sockaddr_storage addr; /* route_resolve 가 시작할 때 채움 (⑥: 요청 중에 blocking DNS 를 부르지 않으려고) */
    socklen_t addrlen;
};

/* -R "/api=host:port,host:port" 한 줄이 만든 규칙 하나 */
struct route
{
    char prefix[MAX_PREFIX + 1]; /* "/api" ("/" 로 끝나지 않음) */
    struct backend be[MAX_BACKENDS];
    int nbe;
    unsigned next; /* Round-Robin 카운터. fork 뒤 worker 마다 따로 돈다 (D4) */
};

/* "/api=127.0.0.1:8001,127.0.0.1:8002" 를 해석한다. 성공 0, 실패 -1 (*err 에 이유).
 * 접두어는 "/" 로 시작·"/" 로 끝나지 않음("/" 단독은 정적 파일을 가려서 거절), 호스트는 영숫자와 . - _, 포트는 1~65535.
 * IPv6 리터럴("[::1]:80") 은 지원하지 않는다. */
int route_parse(const char *spec, struct route *r, const char **err);

/* "host:port,host:port" 목록만 해석해 r->be 를 채운다 (prefix 는 건드리지 않음). L4 중계(-L)가 쓴다.
 * 호스트는 영숫자와 . - _, 포트 1~65535, 최대 8개. 성공 0, 실패 -1 (*err). r 은 호출한 쪽이 0 으로 비워 둘 것 */
int backends_parse(const char *list, struct route *r, const char **err);

/* 모든 백엔드의 호스트 이름을 IPv4 주소로 한 번 해석해 둔다 (시작할 때, fork 전에). 성공 0, 실패 -1 (*err) */
int route_resolve(struct route *routes, int n, const char **err);

/* uri 가 접두어와 "경로 조각 단위" 로 맞는가 ("/api", "/api/x", "/api?q" 는 예, "/apix" 는 아니오). 대소문자 구분 */
int route_matches(const struct route *r, const char *uri);

/* 규칙 n 개 중 uri 에 맞는 것. 여럿이면 접두어가 가장 긴 것. 없으면 NULL (→ 정적 파일) */
struct route *route_find(struct route *routes, int n, const char *uri);

/* Round-Robin: 이번 요청이 시작할 백엔드 번호를 돌려주고 카운터를 하나 올린다.
 * 연결이 실패하면 호출한 쪽이 (start + 1) % nbe, ... 로 다음 백엔드를 시도한다 (D4) */
int route_next_start(struct route *r);

#endif
