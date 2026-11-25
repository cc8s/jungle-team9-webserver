#ifndef PATH_H
#define PATH_H

#include <stddef.h>

/* 퍼센트 디코딩 한 번 ("%2e" → "."). 쿼리('?' 뒤)는 버린다. '+' 는 경로에서는 그냥 '+'.
   성공 0, 잘못된 %XX·디코딩 결과에 NUL·out 부족이면 -1 */
int url_decode_path(const char *uri, char *out, size_t cap);

/* 경로에 ".." 조각이 있는가 ("/a/../b" 는 예, "/a..b" 는 아니오) */
int has_dotdot_segment(const char *path);

/* URI 를 정적 루트 안의 실제 파일 경로로 바꾼다 (D6).
   root_real 은 realpath() 로 미리 바꿔 둔 루트. 디렉터리면 그 안의 index.html.
   성공 0 (out 에 실제 경로), 실패면 응답할 상태 코드 400 / 403 / 404 / 500 */
int resolve_path(const char *root_real, const char *uri, char *out, size_t cap);

#endif
