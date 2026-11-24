#ifndef HTTP_GET_H
#define HTTP_GET_H
#include <stdint.h>

typedef struct {
    int file;
    const char *mime;
    uintmax_t length;
} get_response;

/* path is decoded/validated and may be modified. Returns an HTTP status.
   On 200 the caller owns response.file and must close it, even on send failure. */
int get_prepare(char *path, get_response *response);
/* Uses the shared response sender; does not close response.file. */
int get_send_body(int fd, const get_response *response, int64_t *deadline);
#endif
