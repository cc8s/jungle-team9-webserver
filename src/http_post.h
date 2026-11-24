#ifndef HTTP_POST_H
#define HTTP_POST_H
#include "json.h"
typedef struct {
    int status;
    json_output body;
} post_response;
int post_is_route(const char *path);
int post_error(post_response *response, int status, const char *code, const char *message);
/* Produces JSON in response.body; caller frees it after transmission. */
int post_prepare(const char *path, const unsigned char *body, size_t length,
                 const unsigned char *type, size_t type_length, post_response *response);
#endif
