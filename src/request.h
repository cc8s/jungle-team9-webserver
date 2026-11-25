#ifndef REQUEST_H
#define REQUEST_H

#include <stddef.h>

#define REQ_MAX_HEADER 8192          /* 요청 라인 + 헤더 전체 상한 (넘으면 400, 요청 라인이 안 끝났으면 414) */
#define REQ_MAX_URI 2048             /* 넘으면 414 */
#define REQ_MAX_BODY (1024 * 1024)   /* Content-Length 상한 (넘으면 413) */
#define REQ_MAX_METHOD 16            /* 넘으면 501 (그렇게 긴 메서드는 모르는 메서드) */
#define REQ_MAX_HOST 255             /* 넘으면 400 (도메인 이름 최대 길이) */

enum parse_result
{
    PARSE_INCOMPLETE, /* 덜 옴: 더 읽고 다시 부른다 */
    PARSE_OK,         /* 다 옴: req 를 채웠다 */
    PARSE_ERROR       /* 잘못됨: req->status 로 응답하고 연결을 닫는다 */
};

struct request
{
    char method[REQ_MAX_METHOD + 1];
    char uri[REQ_MAX_URI + 1];
    int minor;                   /* HTTP/1.x 의 x (0 또는 1) */
    char host[REQ_MAX_HOST + 1]; /* Host 헤더가 없으면 "" */
    int keep_alive;              /* 응답 후 연결을 유지해도 되는가 (⑤ 에서는 무시하고 항상 close) */
    size_t content_length;
    const char *body;            /* buf 안을 가리킨다 (복사하지 않음) */
    size_t header_len;           /* 요청 라인 + 헤더 + 빈 줄의 바이트 수 */
    size_t total_len;            /* header_len + content_length = 이 요청이 차지한 바이트 수 */
    int status;                  /* PARSE_ERROR 일 때 돌려줄 상태 코드 (400/413/414/501/505) */
};

/* buf 의 앞 len 바이트를 요청 하나로 해석한다. buf 는 NUL 로 끝나지 않아도 된다.
   상태를 저장하지 않으므로, 덜 왔으면 바이트를 더 붙여 처음부터 다시 부르면 된다. */
enum parse_result parse_request(const char *buf, size_t len, struct request *req);

#endif
