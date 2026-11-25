#include "upload_id.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
static void *issue(void *data) { assert(!upload_id_next(data, 96)); return NULL; }
int main(void)
{
    upload_id_context context = {.worker=7, .sequence=UINT64_MAX};
    char final[96], other[96];
    assert(!upload_id_format_next(&context, final, sizeof(final)));
    assert(context.exhausted && upload_id_format_next(&context, other, sizeof(other)) < 0);
    char a[96], b[96]; pthread_t ta, tb;
    assert(!pthread_create(&ta, NULL, issue, a)); assert(!pthread_create(&tb, NULL, issue, b));
    pthread_join(ta, NULL); pthread_join(tb, NULL);
    assert(strcmp(a,b));
    puts("Upload ID: separate thread contexts and uint64 exhaustion passed");
}
