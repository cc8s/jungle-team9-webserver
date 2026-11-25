#ifdef __linux__
#include "thread_pool.h"
#include <sys/socket.h>
#include <errno.h>
#include <assert.h>
static int submitted, read_error;
static int capture_submit(thread_pool *pool, pool_job *job)
{
    (void)pool; (void)job; submitted++; return 0;
}
static ssize_t controlled_recv(int fd, void *buffer, size_t length, int flags)
{
    if(read_error) {errno=read_error;return -1;}
    return recv(fd,buffer,length,flags);
}
#define thread_pool_submit capture_submit
#define recv controlled_recv
#define event_loop_run receive_test_event_loop_run
#include "../src/event_loop.c"
#undef thread_pool_submit
#undef recv
static void run_case(const char *input, int error, int dispatch_expected)
{
    int pair[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK,0,pair));
    connection c={.fd=pair[0],.state=HEADERS,.job={.execute=execute_job},
                  .response={.file=-1}};
    event_loop loop={.epoll=-1,.active=1,.config={.buffer_budget=1048576}};
    if(input) {
        c.buffer.capacity=4096;c.buffer.length=strlen(input);
        c.buffer.data=malloc(c.buffer.capacity);assert(c.buffer.data);
        memcpy(c.buffer.data,input,c.buffer.length);
        loop.buffer_capacity=c.buffer.capacity;
    }
    assert(!shutdown(pair[1],SHUT_WR));
    submitted=0;read_error=error;
    receive_request(&loop,&c);
    assert(submitted==dispatch_expected);
    if(dispatch_expected) {
        assert(c.state==PREPARING);
        if(strstr(input,"\r\n\r\n")) assert(c.request.error==HTTP_NO_ERROR);
        else assert(c.request.error==HTTP_INCOMPLETE_REQUEST);
        close_connection(&loop,&c); /* Captured job was never sent to a worker. */
    } else {
        assert(c.state==FREE && !loop.active && !loop.buffer_capacity);
        errno=0;assert(fcntl(pair[0],F_GETFD)<0 && errno==EBADF);
    }
    close(pair[1]);read_error=0;
}
int main(void)
{
    run_case(NULL,0,0);
    run_case(NULL,ECONNRESET,0);
    run_case("GET / HTTP/1.1\r\nHost:",EIO,0);
    run_case("GET / HTTP/1.1\r\nHost:",0,1);
    run_case("GET / HTTP/1.1\r\nHost: x\r\n\r\n",0,1);
    puts("Receive close: empty EOF/read errors bypass workers; partial EOF and complete half-close dispatch");
}
#else
#include <stdio.h>
int main(void) {puts("SKIP: epoll receive close requires Linux");}
#endif
