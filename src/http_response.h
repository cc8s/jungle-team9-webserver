#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H
#include "http_request.h"
typedef struct {
    int io_error;
    int close_connection;
} http_response_result;
/* Persistent response: close on force_close, any error status, or I/O failure. */
http_response_result http_respond(int fd, const http_buffer *buffer,
                                  const http_request *request, int force_close);
/* Invalid field ranges yield 500; oversized target yields 414.
   Error responses do not require buffer.
   fd must be O_NONBLOCK. Sends one response; caller owns socket and buffer. */
int http_send_response(int fd, const http_buffer *buffer, const http_request *request);
/* Common partial-write/timeout loop shared by memory and GET file bodies.
   fd must be O_NONBLOCK; deadline is updated only after send progress. */
int http_send_all(int fd, const void *data, size_t length, int64_t *deadline);
#define HTTP_FILE_CHUNK 16384
typedef struct {
    char header[512], fixed_body[128];
    size_t header_length, header_sent, body_length, body_sent;
    unsigned char *body; /* NULL means fixed_body; owned allocation otherwise. */
    int file, close_connection;
    uintmax_t file_left;
    unsigned char chunk[HTTP_FILE_CHUNK];
    size_t chunk_length, chunk_sent;
} http_prepared_response;
/* Preparation and file reads run in workers; neither function touches a socket. */
int http_prepare_response(const http_buffer *buffer, const http_request *request,
                          int force_close, http_prepared_response *response);
void http_prepare_unavailable(http_prepared_response *response);
int http_prepare_file_chunk(http_prepared_response *response);
void http_prepared_dispose(http_prepared_response *response);
#endif
