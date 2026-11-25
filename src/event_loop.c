#include "event_loop.h"
#ifdef __linux__
#include "thread_pool.h"
#include "http_response.h"
#include "http_connection.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#define LISTENER_TOKEN UINT64_MAX
#define WAKE_TOKEN (UINT64_MAX-1)
#define IO_BUDGET 65536
#define ACCEPT_BUDGET 32
/* One loop owns all sockets, receive buffers and epoll registrations.
   JOB states lend only response preparation/file reads to a worker. Completion
   queue publication transfers those fields back before the loop reads them. */
typedef enum { FREE, HEADERS, BODY, IDLE, DISCARD, PREPARING, READING, READ_PENDING, SENDING } connection_state;
typedef struct connection {
    pool_job job;
    int fd, registered, result;
    uint32_t generation;
    size_t index;
    connection_state state;
    http_buffer buffer;
    http_request request;
    http_prepared_response response;
    unsigned requests;
    size_t discard_left;
    int64_t deadline;
    int force_close;
} connection;
typedef struct {
    server_config config;
    int epoll, listener, wake, listener_registered, shutting;
    size_t active, buffer_capacity;
    connection *connections;
    thread_pool pool;
} event_loop;
static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t signal_fd = -1;
static void on_signal(int signal_number)
{
    (void)signal_number;
    int saved = errno;
    stop_requested = 1;
    uint64_t one=1;
    if (signal_fd >= 0) { ssize_t ignored=write(signal_fd, &one, sizeof(one)); (void)ignored; }
    errno = saved;
}
static void wake_loop(void *argument)
{
    int fd = *(int *)argument;
    uint64_t one=1;
    ssize_t n;
    do { n=write(fd,&one,sizeof(one)); } while(n<0 && errno==EINTR);
    /* EAGAIN means the counter is already readable; completions remain queued. */
}
static uint64_t token(connection *c)
{
    return ((uint64_t)c->generation << 32) | (c->index+1);
}
static int watch(event_loop *loop, connection *c, uint32_t events)
{
    struct epoll_event event={.events=events, .data.u64=token(c)};
    int result=epoll_ctl(loop->epoll,c->registered?EPOLL_CTL_MOD:EPOLL_CTL_ADD,c->fd,&event);
    if (!result) c->registered=1;
    return result;
}
static void unwatch(event_loop *loop, connection *c)
{
    if(c->registered) (void)epoll_ctl(loop->epoll,EPOLL_CTL_DEL,c->fd,NULL);
    c->registered=0;
}
static void close_connection(event_loop *loop, connection *c)
{
    if(c->state==FREE) return;
    unwatch(loop,c);
    close(c->fd);
    loop->buffer_capacity-=c->buffer.capacity;
    free(c->buffer.data);
    http_prepared_dispose(&c->response);
    c->buffer=(http_buffer){0};
    c->state=FREE;
    c->fd=-1;
    loop->active--;
}
static int update_listener(event_loop *loop)
{
    int enable=!loop->shutting && loop->active<loop->config.max_connections;
    if(enable==loop->listener_registered) return 0;
    struct epoll_event event={.events=EPOLLIN, .data.u64=LISTENER_TOKEN};
    int r=epoll_ctl(loop->epoll,enable?EPOLL_CTL_ADD:EPOLL_CTL_DEL,loop->listener,
                    enable?&event:NULL);
    if(!r) loop->listener_registered=enable;
    return r;
}
static void execute_job(pool_job *job)
{
    connection *c=(connection *)job;
    if(c->state==PREPARING) {
        c->result=http_prepare_response(&c->buffer,&c->request,c->force_close,&c->response);
        if(!c->result && c->response.file>=0)
            c->result=http_prepare_file_chunk(&c->response);
    } else {
        c->result=http_prepare_file_chunk(&c->response);
    }
}
static int start_sending(event_loop *loop, connection *c)
{
    c->state=SENDING;
    int64_t now=http_now_ms();
    if(now<0) return -1;
    c->deadline=now+5000;
    return watch(loop,c,EPOLLOUT);
}
static void dispatch(event_loop *loop, connection *c)
{
    c->force_close=c->request.connection_close ||
                    c->requests+1==HTTP_CONNECTION_REQUEST_LIMIT;
    unwatch(loop,c);
    c->state=PREPARING;
    if(thread_pool_submit(&loop->pool,&c->job)) {
        /* A small fixed response bypasses the full worker queue. */
        http_prepare_unavailable(&c->response);
        if(start_sending(loop,c)<0) close_connection(loop,c);
    }
}
static int grow_buffer(event_loop *loop, connection *c)
{
    size_t old=c->buffer.capacity;
    size_t capacity=old?old*2:4096;
    if(capacity>HTTP_REQUEST_LIMIT) capacity=HTTP_REQUEST_LIMIT;
    if(capacity<=old || capacity-old>loop->config.buffer_budget-loop->buffer_capacity)
        return -1;
    void *data=realloc(c->buffer.data,capacity);
    if(!data) return -1;
    c->buffer.data=data;
    c->buffer.capacity=capacity;
    loop->buffer_capacity+=capacity-old;
    return 0;
}
static void consume(http_buffer *buffer,size_t length)
{
    buffer->length-=length;
    memmove(buffer->data,buffer->data+length,buffer->length);
}
static void receive_request(event_loop *loop, connection *c)
{
    size_t budget=IO_BUDGET;
    while(budget) {
        if(c->state!=IDLE) {
            http_parse_result parsed=http_parse_request(c->buffer.data,c->buffer.length,&c->request);
            if(parsed!=HTTP_NEED_MORE) { dispatch(loop,c); return; }
            if(c->request.headers_complete && c->state!=BODY) {
                c->state=BODY;
                int64_t now=http_now_ms();
                if(now<0) {close_connection(loop,c);return;}
                c->deadline=now+HTTP_BODY_TIMEOUT_MS;
            }
        }
        if(c->buffer.length==c->buffer.capacity && grow_buffer(loop,c)<0) {
            close_connection(loop,c);return;
        }
        size_t count=c->buffer.capacity-c->buffer.length;
        if(!c->request.headers_complete && count>HTTP_HEADER_LIMIT-c->buffer.length)
            count=HTTP_HEADER_LIMIT-c->buffer.length;
        if(count>budget) count=budget;
        if(!count) {c->request.error=HTTP_HEADERS_TOO_LARGE;dispatch(loop,c);return;}
        ssize_t n=recv(c->fd,c->buffer.data+c->buffer.length,count,0);
        if(n<0 && errno==EINTR) continue;
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return;
        if(n<=0) {
            /* Empty EOF has no HTTP request to answer. Real read errors mean
               the transport failed; neither case should consume a worker. */
            if(n<0 || c->state==IDLE || !c->buffer.length) {
                close_connection(loop,c);return;
            }
            /* A partial request followed by a write-half close can still
               receive the existing incomplete-request error response. */
            c->request.error=HTTP_INCOMPLETE_REQUEST;
            c->request.connection_close=1;
            dispatch(loop,c);return;
        }
        if(c->state==IDLE) {
            int64_t now=http_now_ms();
            if(now<0) {close_connection(loop,c);return;}
            c->state=HEADERS;c->deadline=now+HTTP_HEADER_TIMEOUT_MS;
        }
        c->buffer.length+=(size_t)n;
        budget-=(size_t)n;
        /* Parse actual bytes even when the last recv consumed the budget. */
        http_parse_result parsed=http_parse_request(c->buffer.data,c->buffer.length,&c->request);
        if(parsed!=HTTP_NEED_MORE) {dispatch(loop,c);return;}
        if(c->request.headers_complete && c->state!=BODY) {
            int64_t now=http_now_ms();
            if(now<0) {close_connection(loop,c);return;}
            c->state=BODY;c->deadline=now+HTTP_BODY_TIMEOUT_MS;
        }
    }
}
static void next_request(event_loop *loop,connection *c)
{
    c->request=(http_request){0};
    int64_t now=http_now_ms();
    if(now<0) {close_connection(loop,c);return;}
    c->state=c->buffer.length?HEADERS:IDLE;
    c->deadline=now+(c->buffer.length?HTTP_HEADER_TIMEOUT_MS:HTTP_IDLE_TIMEOUT_MS);
    if(watch(loop,c,EPOLLIN)<0) {close_connection(loop,c);return;}
    if(c->buffer.length) receive_request(loop,c);
}
static void discard_body(event_loop *loop,connection *c)
{
    unsigned char discard[HTTP_FILE_CHUNK];
    size_t budget=IO_BUDGET;
    while(c->discard_left && budget) {
        size_t count=c->discard_left<sizeof(discard)?c->discard_left:sizeof(discard);
        if(count>budget) count=budget;
        ssize_t n=recv(c->fd,discard,count,0);
        if(n<0 && errno==EINTR) continue;
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return;
        if(n<=0) {close_connection(loop,c);return;}
        c->discard_left-=(size_t)n; budget-=(size_t)n;
    }
    if(!c->discard_left) next_request(loop,c);
}
static void response_finished(event_loop *loop,connection *c)
{
    if(c->response.close_connection || loop->shutting) {close_connection(loop,c);return;}
    c->requests++;
    int get=c->request.method.length==3 &&
        !memcmp(c->buffer.data+c->request.method.offset,"GET",3);
    c->discard_left=get?c->request.content_length:0;
    consume(&c->buffer,c->request.consumed);
    http_prepared_dispose(&c->response);
    size_t buffered=c->discard_left<c->buffer.length?c->discard_left:c->buffer.length;
    if(buffered) consume(&c->buffer,buffered);
    c->discard_left-=buffered;
    if(!c->discard_left) {next_request(loop,c);return;}
    int64_t now=http_now_ms();
    if(now<0) {close_connection(loop,c);return;}
    c->state=DISCARD; c->deadline=now+HTTP_BODY_TIMEOUT_MS;
    if(watch(loop,c,EPOLLIN)<0) close_connection(loop,c);
}
static void submit_read(event_loop *loop,connection *c)
{
    unwatch(loop,c);
    c->state=READING;
    if(thread_pool_submit(&loop->pool,&c->job)) c->state=READ_PENDING;
}
static void send_response(event_loop *loop,connection *c)
{
    size_t budget=IO_BUDGET;
    http_prepared_response *r=&c->response;
    while(budget) {
        const unsigned char *data;
        size_t count,*position;
        if(r->header_sent<r->header_length) {
            data=(unsigned char *)r->header;count=r->header_length;position=&r->header_sent;
        } else if(r->file>=0) {
            if(r->chunk_sent==r->chunk_length) {
                if(r->file_left) submit_read(loop,c);
                else response_finished(loop,c);
                return;
            }
            data=r->chunk;count=r->chunk_length;position=&r->chunk_sent;
        } else {
            if(r->body_sent==r->body_length) {response_finished(loop,c);return;}
            data=r->body?r->body:(unsigned char *)r->fixed_body;
            count=r->body_length;position=&r->body_sent;
        }
        size_t length=count-*position;
        if(length>budget) length=budget;
        ssize_t n=send(c->fd,data+*position,length,MSG_NOSIGNAL);
        if(n<0 && errno==EINTR) continue;
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return;
        if(n<=0) {close_connection(loop,c);return;}
        *position+=(size_t)n;budget-=(size_t)n;
        int64_t now=http_now_ms();
        if(now<0) {close_connection(loop,c);return;}
        c->deadline=now+5000;
    }
}
static void collect_completions(event_loop *loop)
{
    pool_job *job;
    while((job=thread_pool_completed(&loop->pool))) {
        connection *c=(connection *)job;
        if(c->result<0) {
            if(c->state==READING) {close_connection(loop,c);continue;}
            http_prepared_dispose(&c->response);
            if(http_prepare_response(NULL,NULL,1,&c->response)<0) {close_connection(loop,c);continue;}
        }
        if(start_sending(loop,c)<0) close_connection(loop,c);
    }
    /* Continuations are existing responses, not new overload requests. */
    for(size_t i=0;i<loop->config.max_connections;i++)
        if(loop->connections[i].state==READ_PENDING) submit_read(loop,&loop->connections[i]);
}
/* Linux may report a pending peer/network error from accept4. It affects that
   connection, not the listening socket. Retry within the normal fairness budget. */
static int transient_accept_error(int error)
{
    switch(error) {
    case ECONNABORTED: case EPROTO: case ENETDOWN: case ENOPROTOOPT:
    case EHOSTDOWN: case ENONET: case EHOSTUNREACH: case EOPNOTSUPP: case ENETUNREACH:
        return 1;
    default: return 0;
    }
}
static int accept_connections(event_loop *loop)
{
    for(int budget=ACCEPT_BUDGET;budget && loop->active<loop->config.max_connections;budget--) {
        int fd=accept4(loop->listener,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC);
        if(fd<0 && errno==EINTR) continue;
        if(fd<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return 0;
        if(fd<0 && transient_accept_error(errno)) continue;
        /* Descriptor exhaustion and broken listener errors fail explicitly rather
           than spin on a permanently readable listener. */
        if(fd<0) return -1;
        int64_t now=http_now_ms();
        if(now<0) {close(fd);return -1;}
        connection *c=NULL;
        for(size_t i=0;i<loop->config.max_connections;i++)
            if(loop->connections[i].state==FREE) {c=&loop->connections[i];break;}
        /* Never reuse a generation after uint32 exhaustion. */
        if(!c || c->generation==UINT32_MAX) {close(fd);return -1;}
        uint32_t generation=c->generation+1;
        size_t index=c->index;
        *c=(connection){.job={.execute=execute_job},.fd=fd,.index=index,
                        .generation=generation,.state=HEADERS,.deadline=now+HTTP_HEADER_TIMEOUT_MS,
                        .response={.file=-1}};
        loop->active++;
        if(watch(loop,c,EPOLLIN)<0) close_connection(loop,c);
    }
    return update_listener(loop);
}
static void begin_shutdown(event_loop *loop)
{
    loop->shutting=1;
    (void)update_listener(loop);
    for(size_t i=0;i<loop->config.max_connections;i++) {
        connection *c=&loop->connections[i];
        if(c->state==HEADERS || c->state==BODY || c->state==IDLE || c->state==DISCARD)
            close_connection(loop,c);
    }
}
static int check_deadlines(event_loop *loop)
{
    int64_t now=http_now_ms(),nearest=now+1000;
    if(now<0) return -1;
    for(size_t i=0;i<loop->config.max_connections;i++) {
        connection *c=&loop->connections[i];
        if(c->state!=HEADERS && c->state!=BODY && c->state!=IDLE &&
           c->state!=DISCARD && c->state!=SENDING) continue;
        if(now>=c->deadline) {
            if(c->state==HEADERS || c->state==BODY) {
                c->request.error=c->state==BODY?HTTP_BODY_TIMEOUT:HTTP_HEADER_TIMEOUT;
                c->request.connection_close=1; dispatch(loop,c);
            } else close_connection(loop,c);
        } else if(c->deadline<nearest) nearest=c->deadline;
    }
    return nearest>now?(int)(nearest-now):0;
}
int event_loop_run(const server_config *config)
{
    event_loop loop={.config=*config,.epoll=-1,.listener=-1,.wake=-1};
    int result=-1,pool_ready=0,handlers=0;
    struct sigaction old_int,old_term;
    loop.connections=calloc(config->max_connections,sizeof(connection));
    if(!loop.connections) goto cleanup;
    for(size_t i=0;i<config->max_connections;i++) {
        loop.connections[i].index=i;loop.connections[i].fd=-1;
        loop.connections[i].response.file=-1;
    }
    loop.epoll=epoll_create1(EPOLL_CLOEXEC);
    loop.wake=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    loop.listener=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(loop.epoll<0 || loop.wake<0 || loop.listener<0) goto cleanup;
    int on=1;
    if(setsockopt(loop.listener,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on))) goto cleanup;
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(8000),
                                .sin_addr.s_addr=htonl(INADDR_ANY)};
    if(bind(loop.listener,(struct sockaddr *)&address,sizeof(address)) || listen(loop.listener,128)) goto cleanup;
    struct epoll_event wake={.events=EPOLLIN,.data.u64=WAKE_TOKEN};
    if(epoll_ctl(loop.epoll,EPOLL_CTL_ADD,loop.wake,&wake) || update_listener(&loop)) goto cleanup;
    stop_requested=0;signal_fd=loop.wake;
    struct sigaction action={.sa_handler=on_signal};sigemptyset(&action.sa_mask);
    if(sigaction(SIGINT,&action,&old_int)) goto cleanup;
    if(sigaction(SIGTERM,&action,&old_term)) {sigaction(SIGINT,&old_int,NULL);goto cleanup;}
    handlers=1;
    if(thread_pool_init(&loop.pool,config->workers,config->queue,wake_loop,&loop.wake)) goto cleanup;
    pool_ready=1;
    fprintf(stderr,"listening :8000 workers=%zu queue=%zu max=%zu buffer=%zu bytes\n",
            config->workers,config->queue,config->max_connections,config->buffer_budget);
    for(;;) {
        if(stop_requested && !loop.shutting) begin_shutdown(&loop);
        collect_completions(&loop);
        if(loop.shutting && !loop.active) {result=0;break;}
        int timeout=check_deadlines(&loop);
        if(timeout<0 || update_listener(&loop)<0) break;
        struct epoll_event events[64];
        int count=epoll_wait(loop.epoll,events,64,timeout);
        if(count<0 && errno==EINTR) continue;
        if(count<0) break;
        for(int i=0;i<count;i++) {
            if(stop_requested && !loop.shutting) begin_shutdown(&loop);
            uint64_t key=events[i].data.u64;
            if(key==WAKE_TOKEN) {
                uint64_t value;
                while(read(loop.wake,&value,sizeof(value))<0 && errno==EINTR) {}
                collect_completions(&loop);continue;
            }
            if(key==LISTENER_TOKEN) {
                if(!loop.shutting && accept_connections(&loop)<0) goto cleanup;
                continue;
            }
            size_t index=(uint32_t)key;
            if(!index || index>config->max_connections) continue;
            connection *c=&loop.connections[index-1];
            if(c->state==FREE || c->generation!=(uint32_t)(key>>32)) continue;
            if(c->state==HEADERS || c->state==BODY || c->state==IDLE) receive_request(&loop,c);
            else if(c->state==DISCARD) discard_body(&loop,c);
            else if(c->state==SENDING) send_response(&loop,c);
        }
    }
cleanup:
    if(result<0) perror("event loop");
    /* No connection storage is released while a worker can still access it. */
    if(pool_ready) thread_pool_destroy(&loop.pool);
    if(loop.connections) for(size_t i=0;i<config->max_connections;i++) close_connection(&loop,&loop.connections[i]);
    if(handlers) {sigaction(SIGINT,&old_int,NULL);sigaction(SIGTERM,&old_term,NULL);}
    signal_fd=-1;
    if(loop.listener>=0) close(loop.listener);
    if(loop.wake>=0) close(loop.wake);
    if(loop.epoll>=0) close(loop.epoll);
    free(loop.connections);
    return result;
}
#else
#include <stdio.h>
int event_loop_run(const server_config *config)
{
    (void)config;
    fprintf(stderr,"epoll server requires Linux; build/run using Docker.\n");
    return -1;
}
#endif
