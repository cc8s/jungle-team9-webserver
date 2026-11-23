#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H
#include "http_request.h"
/* Sends one response; caller retains ownership of socket and receive buffer. */
int http_send_response(int fd, const http_buffer *buffer, const http_request *request);
#endif
