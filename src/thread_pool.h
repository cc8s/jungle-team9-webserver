#ifndef THREAD_POOL_H
#define THREAD_POOL_H
#include <stddef.h>
#include <pthread.h>
typedef struct pool_job pool_job;
struct pool_job {
    void (*execute)(pool_job *job);
    pool_job *next;
};
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t available;
    pthread_t *threads;
    void *contexts;
    pool_job **queue;
    size_t count, capacity, head, workers, started;
    pool_job *completed_head, *completed_tail;
    int stopping;
    void (*notify)(void *context);
    void *notify_context;
} thread_pool;
/* On failure init cleans up all partially initialized resources. */
int thread_pool_init(thread_pool *pool, size_t workers, size_t capacity,
                     void (*notify)(void *), void *context);
/* Nonblocking: 0 accepted, 1 full, -1 stopping. Caller owns rejected jobs. */
int thread_pool_submit(thread_pool *pool, pool_job *job);
/* Returns one completion. Queue/condition locks never cover execute(). */
pool_job *thread_pool_completed(thread_pool *pool);
/* Only after the owner has drained accepted jobs and their completions. */
void thread_pool_destroy(thread_pool *pool);
#endif
