#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H
#include <stddef.h>

#define HTTP_LINE_LIMIT 8192
#define HTTP_HEADER_LIMIT 32768
#define HTTP_BODY_LIMIT 1048576
#define HTTP_REQUEST_LIMIT (HTTP_HEADER_LIMIT + HTTP_BODY_LIMIT)

typedef struct { size_t offset, length; } http_field;
typedef enum { HTTP_NEED_MORE, HTTP_COMPLETE, HTTP_PARSE_ERROR } http_parse_result;
typedef enum {
    HTTP_NO_ERROR, HTTP_BAD_REQUEST, HTTP_LINE_TOO_LONG,
    HTTP_HEADERS_TOO_LARGE, HTTP_BODY_TOO_LARGE,
    HTTP_UNSUPPORTED_TRANSFER, HTTP_UNSUPPORTED_VERSION,
    HTTP_READ_ERROR, HTTP_INCOMPLETE_REQUEST, HTTP_NO_MEMORY
} http_request_error;

typedef struct {
    http_field method, target, version, host, connection;
    size_t header_length, content_length, consumed;
    int headers_complete;
    http_request_error error;
} http_request;

typedef struct {
    unsigned char *data;
    size_t length, capacity;
} http_buffer;

/* Initialize request to zero. Keep the same accumulated buffer between calls. */
http_parse_result http_parse_request(const unsigned char *data, size_t length,
                                     http_request *request);
/* Blocking I/O; caller owns fd and frees buffer.data after response handling. */
http_parse_result http_receive_request(int fd, http_buffer *buffer,
                                       http_request *request);
#endif
