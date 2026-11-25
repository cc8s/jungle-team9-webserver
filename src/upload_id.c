#include "upload_id.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
static _Thread_local upload_id_context local;
static atomic_uint_fast64_t fallback = ATOMIC_VAR_INIT(UINT64_C(1) << 32);
void upload_id_set_worker(uint64_t worker)
{
    local = (upload_id_context){.worker = worker};
}
int upload_id_format_next(upload_id_context *context, char *output, size_t capacity)
{
    if (!context || !context->worker || context->exhausted) return -1;
    int n = snprintf(output, capacity, "%jd_%" PRIu64 "_%" PRIu64,
                     (intmax_t)getpid(), context->worker, context->sequence);
    if (n < 0 || (size_t)n >= capacity) return -1;
    if (context->sequence == UINT64_MAX) context->exhausted = 1;
    else context->sequence++;
    return 0;
}
int upload_id_next(char *output, size_t capacity)
{
    if (!local.worker) {
        uint_fast64_t value = atomic_load(&fallback);
        for (;;) {
            if (value == UINT64_MAX) return -1;
            if (atomic_compare_exchange_weak(&fallback, &value, value + 1)) break;
        }
        local.worker = value;
    }
    return upload_id_format_next(&local, output, capacity);
}
