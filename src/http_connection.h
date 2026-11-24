#ifndef HTTP_CONNECTION_H
#define HTTP_CONNECTION_H
#include <stdint.h>
#define HTTP_IDLE_TIMEOUT_MS 5000
#define HTTP_CONNECTION_REQUEST_LIMIT 100
/* fd must be O_NONBLOCK. Caller closes fd after this function returns. */
void handle_connection(int fd, int64_t accepted_at);
#endif
