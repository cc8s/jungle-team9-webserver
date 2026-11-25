#ifndef UPLOAD_ID_H
#define UPLOAD_ID_H
#include <stddef.h>
#include <stdint.h>
typedef struct { uint64_t worker, sequence; int exhausted; } upload_id_context;
/* Worker IDs are 1..N; fallback callers get IDs in a disjoint high range. */
void upload_id_set_worker(uint64_t worker);
int upload_id_next(char *output, size_t capacity);
/* Explicit context permits boundary tests without issuing billions of IDs. */
int upload_id_format_next(upload_id_context *context, char *output, size_t capacity);
#endif
