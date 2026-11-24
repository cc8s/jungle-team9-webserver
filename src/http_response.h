#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H
#include "http_request.h"
/* Invalid field ranges yield 500; oversized target yields 414.
   Error responses do not require buffer.
   fd must be O_NONBLOCK. Sends one response; caller owns socket and buffer. */
int http_send_response(int fd, const http_buffer *buffer, const http_request *request);
/* Common partial-write/timeout loop shared by memory and GET file bodies.
   fd must be O_NONBLOCK; deadline is updated only after send progress. */
int http_send_all(int fd, const void *data, size_t length, int64_t *deadline);
#endif
