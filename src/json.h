#ifndef JSON_H
#define JSON_H
#include <stddef.h>

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } json_type;
typedef struct {
    unsigned char *data;
    size_t length;
} json_bytes;
typedef struct json_value json_value;
struct json_value {
    json_type type;
    json_bytes text; /* Strings are decoded UTF-8; numbers retain their spelling. */
    json_bytes key;  /* Object member name; length-aware, including embedded NUL. */
    json_value *child, *next;
    int boolean;
};
typedef struct {
    char *data;
    size_t length, capacity;
} json_output;
/* Return HTTP-like status: 0 success, 400 syntax, 413 limits, 500 allocation. */
int json_parse(const unsigned char *data, size_t length, json_value **value);
void json_free(json_value *value);
const json_value *json_member(const json_value *object, const char *name);
int json_write(json_output *out, const json_value *value);
int json_quote(json_output *out, const unsigned char *text, size_t length);
int json_append(json_output *out, const char *text, size_t length);
int form_parse(const unsigned char *data, size_t length, json_value **value);
#endif
