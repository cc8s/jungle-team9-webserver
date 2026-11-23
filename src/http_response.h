#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H
#include "http_request.h"
/* fd must be O_NONBLOCK. Sends one response; caller owns socket and buffer. */
int http_send_response(int fd, const http_buffer *buffer, const http_request *request);
#endif
