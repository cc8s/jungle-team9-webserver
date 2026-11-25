#ifndef PROXY_H
#define PROXY_H

#include <stddef.h>

#include "request.h"
#include "route.h"

/* 손님 요청 헤더를 백엔드로 보낼 형태로 다시 쓴다 (순수 함수, 단위 테스트 가능).
 * - 요청 라인과 URI 는 그대로 (접두어를 붙인 채 전달, D4)
 * - Connection / Keep-Alive / Proxy-Connection 은 버리고 "Connection: close" 로 (⑤ 는 연결을 재사용하지 않음)
 * - 손님이 보낸 X-Forwarded-For 는 버리고 우리가 본 주소 하나로 (위조 방지)
 * hdr 은 buf 의 앞 hdr_len 바이트 (요청 라인 + 헤더 + 빈 줄). 반환: 쓴 바이트 수, 자리가 모자라면 -1 */
int proxy_build_request(const char *hdr, size_t hdr_len, const char *client_ip, char *out, size_t cap);

/* 요청 하나를 백엔드로 전달하고 응답을 손님에게 그대로 중계한다 (성공이든 오류든 응답을 다 보냄).
 * buf 는 read_request 가 요청을 읽어 둔 버퍼 (req.header_len 까지가 헤더, 그 뒤가 본문).
 * 오류: 경로에 ".." 면 403, 잘못된 % 면 400, 백엔드에 전부 연결 실패 502, 응답이 하나도 안 오면 504.
 * timeout_sec 은 연결 시도마다·응답 바이트 사이마다 (활동이 없는 시간).
 * 반환: 손님에게 보낸 상태 코드 (로그용, 백엔드 응답이면 그 코드), 손님이 중간에 끊으면 -1 */
int proxy_forward(int clientfd, const char *buf, const struct request *req, struct route *rt, const char *client_ip,
                  int timeout_sec);

#endif
