#include "thread_pool.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
static int creates,joins,allocation,fail_allocation;
static void *controlled_calloc(size_t count,size_t size)
{
    if(++allocation==fail_allocation) return NULL;
    return calloc(count,size);
}
static int controlled_create(pthread_t *thread,const pthread_attr_t *attr,
                             void *(*entry)(void *),void *argument)
{
    if(++creates==2) return EAGAIN;
    return pthread_create(thread,attr,entry,argument);
}
static int controlled_join(pthread_t thread,void **result)
{
    joins++;return pthread_join(thread,result);
}
void failure_pool_destroy(thread_pool *pool);
#define calloc controlled_calloc
#define pthread_create controlled_create
#define pthread_join controlled_join
#define thread_pool_init failure_pool_init
#define thread_pool_submit failure_pool_submit
#define thread_pool_completed failure_pool_completed
#define thread_pool_destroy failure_pool_destroy
#include "../src/thread_pool.c"
int main(void)
{
    thread_pool pool;
    for(int i=1;i<=3;i++) {
        fail_allocation=i;allocation=creates=joins=0;
        assert(failure_pool_init(&pool,4,8,NULL,NULL)<0);
        assert(!creates && !joins && errno==ENOMEM);
    }
    fail_allocation=0;allocation=creates=joins=0;
    assert(failure_pool_init(&pool,4,8,NULL,NULL)<0);
    assert(creates==2 && joins==1 && errno==EAGAIN);
    puts("Thread pool failure: allocation rollback and partial worker creation join passed");
}
