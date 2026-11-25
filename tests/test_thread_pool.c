#include "thread_pool.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include <stddef.h>
typedef struct { pool_job job; atomic_int *started, *release; int done; } test_job;
static void execute(pool_job *job)
{
    test_job *test = (test_job *)job;
    atomic_fetch_add(test->started, 1);
    while (!atomic_load(test->release)) usleep(1000);
    test->done=1;
}
int main(void)
{
    thread_pool pool; atomic_int started=0, release=0;
    assert(!thread_pool_init(&pool, 2, 2, NULL, NULL));
    test_job jobs[5];
    for(int i=0;i<5;i++) jobs[i]=(test_job){{.execute=execute},&started,&release,0};
    assert(!thread_pool_submit(&pool,&jobs[0].job));
    assert(!thread_pool_submit(&pool,&jobs[1].job));
    for(int i=0;atomic_load(&started)!=2 && i<2000;i++) usleep(1000);
    assert(atomic_load(&started)==2);
    assert(!thread_pool_submit(&pool,&jobs[2].job));
    assert(!thread_pool_submit(&pool,&jobs[3].job));
    assert(thread_pool_submit(&pool,&jobs[4].job)==1);
    atomic_store(&release,1);
    int count=0;
    for(int i=0;count<4 && i<2000;i++) {
        pool_job *job;
        while((job=thread_pool_completed(&pool))) { assert(((test_job*)job)->done); count++; }
        usleep(1000);
    }
    assert(count==4 && !jobs[4].done);
    thread_pool_destroy(&pool);
    puts("Thread pool: parallel execution, bounded waiting queue and completion drain passed");
}
