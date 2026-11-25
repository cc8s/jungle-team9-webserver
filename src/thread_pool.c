#include "thread_pool.h"
#include "upload_id.h"
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
typedef struct { thread_pool *pool; uint64_t number; } worker_context;
static void *worker_main(void *argument)
{
    worker_context *context = argument;
    thread_pool *pool = context->pool;
    upload_id_set_worker(context->number);
    for (;;) {
        pthread_mutex_lock(&pool->mutex);
        while (!pool->count && !pool->stopping)
            pthread_cond_wait(&pool->available, &pool->mutex);
        if (!pool->count && pool->stopping) {
            pthread_mutex_unlock(&pool->mutex); return NULL;
        }
        pool_job *job = pool->queue[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        pool->count--;
        pthread_mutex_unlock(&pool->mutex);
        /* The caller's job and its data remain owned until completion retrieval. */
        job->execute(job);
        job->next = NULL;
        pthread_mutex_lock(&pool->mutex);
        if (pool->completed_tail) pool->completed_tail->next = job;
        else pool->completed_head = job;
        pool->completed_tail = job;
        pthread_mutex_unlock(&pool->mutex);
        if (pool->notify) pool->notify(pool->notify_context);
    }
}
int thread_pool_init(thread_pool *pool, size_t workers, size_t capacity,
                     void (*notify)(void *), void *context)
{
    *pool = (thread_pool){.capacity=capacity, .workers=workers,
                          .notify=notify, .notify_context=context};
    if (!workers || !capacity) {errno=EINVAL;return -1;}
    int error=pthread_mutex_init(&pool->mutex, NULL);
    if (error) {errno=error;return -1;}
    error=pthread_cond_init(&pool->available, NULL);
    if (error) {
        pthread_mutex_destroy(&pool->mutex);errno=error;return -1;
    }
    pool->threads = calloc(workers, sizeof(*pool->threads));
    pool->contexts = calloc(workers, sizeof(worker_context));
    pool->queue = calloc(capacity, sizeof(*pool->queue));
    if (!pool->threads || !pool->contexts || !pool->queue) {error=ENOMEM;goto fail;}
    worker_context *contexts = pool->contexts;
    for (size_t i=0; i<workers; i++) {
        contexts[i] = (worker_context){pool, i+1};
        error=pthread_create(&pool->threads[i], NULL, worker_main, &contexts[i]);
        if (error) goto fail;
        pool->started++;
    }
    return 0;
fail:
    thread_pool_destroy(pool);
    errno=error;
    return -1;
}
int thread_pool_submit(thread_pool *pool, pool_job *job)
{
    pthread_mutex_lock(&pool->mutex);
    int result = pool->stopping ? -1 : pool->count == pool->capacity ? 1 : 0;
    if (!result) {
        pool->queue[(pool->head + pool->count) % pool->capacity] = job;
        pool->count++;
        pthread_cond_signal(&pool->available);
    }
    pthread_mutex_unlock(&pool->mutex);
    return result;
}
pool_job *thread_pool_completed(thread_pool *pool)
{
    pthread_mutex_lock(&pool->mutex);
    pool_job *job = pool->completed_head;
    if (job) {
        pool->completed_head = job->next;
        if (!pool->completed_head) pool->completed_tail = NULL;
    }
    pthread_mutex_unlock(&pool->mutex);
    return job;
}
void thread_pool_destroy(thread_pool *pool)
{
    pthread_mutex_lock(&pool->mutex);
    pool->stopping = 1;
    pthread_cond_broadcast(&pool->available);
    pthread_mutex_unlock(&pool->mutex);
    for (size_t i=0; i<pool->started; i++) pthread_join(pool->threads[i], NULL);
    free(pool->threads); free(pool->contexts); free(pool->queue);
    pthread_cond_destroy(&pool->available);
    pthread_mutex_destroy(&pool->mutex);
}
