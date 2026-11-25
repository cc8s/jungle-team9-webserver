#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "path.h"

static char root[PATH_MAX], outside[PATH_MAX], out[PATH_MAX];

static void make_file(const char *dir, const char *name, const char *text)
{
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    fputs(text, f);
    fclose(f);
}

/* 오류 코드, 또는 성공이면 0 이고 out 이 root/want 인가 */
static int resolves_to(const char *uri, const char *want)
{
    char expect[PATH_MAX * 2];
    snprintf(expect, sizeof(expect), "%s/%s", root, want);
    return resolve_path(root, uri, out, sizeof(out)) == 0 && strcmp(out, expect) == 0;
}
#define STATUS(uri) resolve_path(root, (uri), out, sizeof(out))

int main(void)
{
    char d[PATH_MAX];

    /* ---- 디코딩 (순수 함수) ---- */
    CHECK(url_decode_path("/a%20b.html", d, sizeof(d)) == 0 && strcmp(d, "/a b.html") == 0);
    CHECK(url_decode_path("/%E D", d, sizeof(d)) == -1);    /* 16진수 아님 */
    CHECK(url_decode_path("/abc%4", d, sizeof(d)) == -1);    /* % 뒤가 모자람 */
    CHECK(url_decode_path("/abc%", d, sizeof(d)) == -1);
    CHECK(url_decode_path("/a%00.html", d, sizeof(d)) == -1); /* NUL */
    CHECK(url_decode_path("/q.html?x=%zz", d, sizeof(d)) == 0 && strcmp(d, "/q.html") == 0); /* 쿼리는 버림 */
    CHECK(url_decode_path("/a+b", d, sizeof(d)) == 0 && strcmp(d, "/a+b") == 0); /* 경로의 + 는 그대로 */
    CHECK(url_decode_path("/%252e", d, sizeof(d)) == 0 && strcmp(d, "/%2e") == 0); /* 한 번만 디코딩 */
    CHECK(url_decode_path("/abcdef", d, 4) == -1);           /* out 부족 */

    /* ---- .. 조각 (순수 함수) ---- */
    CHECK(has_dotdot_segment("/.."));
    CHECK(has_dotdot_segment("/../x"));
    CHECK(has_dotdot_segment("/a/../../x"));
    CHECK(!has_dotdot_segment("/a..b.html"));
    CHECK(!has_dotdot_segment("/..a/b.."));
    CHECK(!has_dotdot_segment("/a/./b"));

    /* ---- 실제 파일 시스템: 임시 디렉터리에 루트와 루트 밖을 만든다 ----
       base/
         www/          ← root
           index.html
           a..b.html
           docs/       (index.html 없음)
           sub/index.html
           escape  → ../secret.txt (루트 밖을 가리키는 링크)
           inner   → index.html    (루트 안을 가리키는 링크)
         www2/x.html   ← 이름이 root 로 시작하지만 밖
         secret.txt */
    char base[] = "/tmp/webserver-test-XXXXXX";
    CHECK(mkdtemp(base) != NULL);
    char tmp[PATH_MAX + 32]; /* root(최대 PATH_MAX) 뒤에 "/escape" 등을 붙이므로 여유를 둔다 */
    snprintf(tmp, sizeof(tmp), "%s/www", base);
    mkdir(tmp, 0755);
    CHECK(realpath(tmp, root) != NULL);
    snprintf(outside, sizeof(outside), "%s/www2", base);
    mkdir(outside, 0755);
    make_file(outside, "x.html", "x");
    make_file(base, "secret.txt", "secret");
    make_file(root, "index.html", "home");
    make_file(root, "a..b.html", "dots");
    snprintf(tmp, sizeof(tmp), "%s/docs", root);
    mkdir(tmp, 0755);
    snprintf(tmp, sizeof(tmp), "%s/sub", root);
    mkdir(tmp, 0755);
    make_file(tmp, "index.html", "sub");
    snprintf(tmp, sizeof(tmp), "%s/escape", root);
    CHECK(symlink("../secret.txt", tmp) == 0);
    snprintf(tmp, sizeof(tmp), "%s/inner", root);
    CHECK(symlink("index.html", tmp) == 0);
    snprintf(tmp, sizeof(tmp), "%s/sib", root); /* 이름이 root 로 "시작만" 하는 www2 를 가리킴 */
    CHECK(symlink("../www2/x.html", tmp) == 0);

    /* 정상 */
    CHECK(resolves_to("/index.html", "index.html"));
    CHECK(resolves_to("/", "index.html"));           /* 디렉터리 → index.html */
    CHECK(resolves_to("/sub", "sub/index.html"));
    CHECK(resolves_to("/sub/", "sub/index.html"));
    CHECK(resolves_to("/a..b.html", "a..b.html"));   /* 점 두 개가 있어도 조각이 아니면 통과 */
    CHECK(resolves_to("/%61..b.html", "a..b.html")); /* %61 = a */
    CHECK(resolves_to("/inner", "index.html"));      /* 루트 안 링크는 허용 */
    CHECK(resolves_to("/index.html?v=1", "index.html"));

    /* 탈출 시도 → 403 */
    CHECK(STATUS("/../secret.txt") == 403);
    CHECK(STATUS("/sub/../../secret.txt") == 403);
    CHECK(STATUS("/%2e%2e/secret.txt") == 403);      /* 디코딩 후 검사 */
    CHECK(STATUS("/..%2fsecret.txt") == 403);        /* %2f = / */
    CHECK(STATUS("/escape") == 403);                 /* 링크로 밖 → realpath 검사가 잡음 */
    CHECK(STATUS("/../www2/x.html") == 403);
    CHECK(STATUS("/sib") == 403); /* ".../www2/x.html" 은 ".../www" 로 시작하지만 조각 단위로는 밖 */

    /* 루트 밖 파일이 "있든 없든" 같은 403 이어야 밖의 파일 존재 여부를 떠볼 수 없다.
       realpath 검사만 있으면 있는 파일은 403, 없는 파일은 404 로 갈려 정보가 샌다 → .. 검사가 이걸 막는다 */
    CHECK(STATUS("/../secret.txt") == 403);
    CHECK(STATUS("/../no-such-file.txt") == 403);

    /* 그 밖 */
    CHECK(STATUS("/a%00.html") == 400);
    CHECK(STATUS("/nope.html") == 404);
    CHECK(STATUS("/index.html/") == 404);            /* 파일 뒤 / → ENOTDIR */
    CHECK(STATUS("/docs/") == 403);                  /* index 없는 디렉터리: 목록 비공개 */
    CHECK(STATUS("/%252e%252e/secret.txt") == 404);  /* 한 번만 디코딩 → "%2e%2e" 라는 이름의 디렉터리를 찾음 */

    /* out 이 모자라면 500 */
    CHECK(resolve_path(root, "/index.html", out, 4) == 500);

    /* 정리 */
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
    CHECK(system(cmd) == 0);

    CHECK_DONE();
}
