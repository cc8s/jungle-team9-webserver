#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H
#include <stddef.h>
typedef struct {
    size_t workers, queue, max_connections, buffer_budget;
} server_config;
/* Linux epoll server, port 8000 and existing www/uploads_storage roots. */
int event_loop_run(const server_config *config);
#endif
