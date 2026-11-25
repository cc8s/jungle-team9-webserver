#ifndef STATIC_H
#define STATIC_H

#include <sys/types.h>

#include "request.h"

/* 정적 경로에서 이 메서드를 처리할 수 있나 (D9).
   GET·HEAD → 0, 아는 다른 메서드 → 405, 모르는 메서드 → 501 */
int static_method_status(const char *method);

/* 정적 응답 준비: 열어 둔 파일 fd(호출한 쪽이 닫음), 크기, Content-Type 을 돌려준다.
   성공 0, 실패면 응답할 상태 코드 (405·501 메서드, 400·403·404·500 경로). 소켓은 건드리지 않아서
   blocking 서버(serve_static)와 이벤트 루프(⑥)가 같이 쓴다 */
int static_open(const char *root_real, const struct request *req, int *fd, off_t *size, const char **type);

/* 요청 하나를 정적 파일로 응답한다 (성공이든 오류든 응답을 다 보냄).
   반환: 보낸 상태 코드 (로그용). 쓰다가 손님이 끊으면 -1 */
int serve_static(int fd, const char *root_real, const struct request *req);

#endif
