#ifndef STATIC_H
#define STATIC_H

#include "request.h"

/* 정적 경로에서 이 메서드를 처리할 수 있나 (D9).
   GET·HEAD → 0, 아는 다른 메서드 → 405, 모르는 메서드 → 501 */
int static_method_status(const char *method);

/* 요청 하나를 정적 파일로 응답한다 (성공이든 오류든 응답을 다 보냄).
   반환: 보낸 상태 코드 (로그용). 쓰다가 손님이 끊으면 -1 */
int serve_static(int fd, const char *root_real, const struct request *req);

#endif
