#include <string.h>

#include "check.h"
#include "mime.h"

#define IS(path, type) (strcmp(mime_type(path), (type)) == 0)

int main(void)
{
    CHECK(IS("/index.html", "text/html; charset=utf-8"));
    CHECK(IS("/a/b/style.css", "text/css; charset=utf-8"));
    CHECK(IS("/app.js", "text/javascript; charset=utf-8"));
    CHECK(IS("/logo.png", "image/png"));
    CHECK(IS("/photo.jpg", "image/jpeg"));
    CHECK(IS("/photo.jpeg", "image/jpeg"));
    CHECK(IS("/icon.svg", "image/svg+xml"));

    CHECK(IS("/PHOTO.JPG", "image/jpeg"));                   /* 대소문자 무시 */
    CHECK(IS("/archive.tar.gz", "application/octet-stream")); /* 마지막 점 기준, gz 는 표에 없음 */
    CHECK(IS("/notes.html.bak", "application/octet-stream")); /* .bak → 실행·렌더링 안 되게 */
    CHECK(IS("/Makefile", "application/octet-stream"));       /* 확장자 없음 */
    CHECK(IS("/.htaccess", "application/octet-stream"));      /* 점으로 시작 = 확장자 아님 */
    CHECK(IS("/dir.html/file", "application/octet-stream"));  /* 점은 디렉터리 이름에만 */
    CHECK(IS("/trailing.", "application/octet-stream"));

    CHECK_DONE();
}
