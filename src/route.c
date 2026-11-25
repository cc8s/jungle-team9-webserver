#include "route.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int valid_port(const char *s)
{
    char *end;
    long v;

    if (*s == '\0')
        return 0;
    v = strtol(s, &end, 10);
    return *end == '\0' && v >= 1 && v <= 65535;
}

static int valid_host(const char *s, size_t n)
{
    if (n == 0 || n > MAX_HOST)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '.' && s[i] != '-' && s[i] != '_')
            return 0;
    return 1;
}

int route_parse(const char *spec, struct route *r, const char **err)
{
    const char *eq = strchr(spec, '=');

    memset(r, 0, sizeof(*r));
    if (eq == NULL || spec[0] != '/') {
        *err = "-R must look like /prefix=host:port[,host:port]";
        return -1;
    }
    size_t plen = (size_t)(eq - spec);
    if (plen == 1 || spec[plen - 1] == '/' || plen > MAX_PREFIX) {
        *err = "-R prefix must not be \"/\" or end with \"/\" (and be at most 128 chars)";
        return -1;
    }
    memcpy(r->prefix, spec, plen);

    /* "host:port,host:port" 를 쉼표로 나눠 하나씩 */
    for (const char *p = eq + 1; *p != '\0';) {
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        const char *colon = memchr(p, ':', n);
        char port[16];

        if (colon == NULL || r->nbe == MAX_BACKENDS || !valid_host(p, (size_t)(colon - p))) {
            *err = "-R backend must be host:port (letters, digits, . - _ only; at most 8 backends)";
            return -1;
        }
        size_t pl = n - (size_t)(colon - p) - 1;
        if (pl == 0 || pl >= sizeof(port) || sizeof(r->be[0].port) <= pl) {
            *err = "-R backend port must be 1-65535";
            return -1;
        }
        memcpy(port, colon + 1, pl);
        port[pl] = '\0';
        if (!valid_port(port)) {
            *err = "-R backend port must be 1-65535";
            return -1;
        }
        memcpy(r->be[r->nbe].host, p, (size_t)(colon - p));
        strcpy(r->be[r->nbe].port, port);
        r->nbe++;
        p = end ? end + 1 : p + n;
        if (end && *p == '\0') { /* "host:80," 처럼 쉼표로 끝남 */
            *err = "-R backend list must not end with a comma";
            return -1;
        }
    }
    if (r->nbe == 0) {
        *err = "-R needs at least one backend";
        return -1;
    }
    return 0;
}

int route_matches(const struct route *r, const char *uri)
{
    size_t n = strlen(r->prefix);

    /* 접두어가 같고, 그다음이 끝·'/'·'?' 일 때만 (조각 단위). 대소문자 구분 */
    return strncmp(uri, r->prefix, n) == 0 && (uri[n] == '\0' || uri[n] == '/' || uri[n] == '?');
}

struct route *route_find(struct route *routes, int n, const char *uri)
{
    struct route *best = NULL;

    for (int i = 0; i < n; i++)
        if (route_matches(&routes[i], uri) && (best == NULL || strlen(routes[i].prefix) > strlen(best->prefix)))
            best = &routes[i];
    return best;
}

int route_next_start(struct route *r)
{
    return (int)(r->next++ % (unsigned)r->nbe);
}
