#ifndef RESPONSE_H
#define RESPONSE_H

#include <stddef.h>
#include <sys/types.h>

/* 상태 코드 → 이유 문구 ("OK", "Not Found" …). 우리가 쓰지 않는 코드는 NULL */
const char *status_text(int status);

/* 상태 라인 + 표준 헤더 + 빈 줄을 buf 에 쓴다. 본문은 쓰지 않는다.
   content_type 이 NULL 이면 Content-Type 을 생략, extra 는 "Allow: GET, HEAD\r\n" 처럼 덧붙일 헤더 (없으면 NULL).
   반환: 쓴 바이트 수. 모르는 상태 코드이거나 cap 이 모자라면 -1 */
int build_header(char *buf, size_t cap, int status, const char *content_type, size_t content_length,
                 int keep_alive, const char *extra);

/* 오류 응답 전체 (헤더 + 고정 문구 HTML). 요청 내용은 본문에 넣지 않는다 (D7). 항상 Connection: close.
   head_only 면 본문을 빼고 헤더만 (HEAD 요청). 반환은 build_header 와 같다 */
int build_error(char *buf, size_t cap, int status, int head_only);

/* n 바이트를 다 쓸 때까지 write 를 반복한다 (부분 쓰기·EINTR 처리). 성공 0, 실패 -1 */
int write_all(int fd, const void *buf, size_t n);

#endif
