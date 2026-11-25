#ifdef __linux__
#include <sys/socket.h>
#include <errno.h>
#include <assert.h>
static int injected_error, calls, repeat;
static int injected_accept4(int fd,struct sockaddr *address,socklen_t *length,int flags)
{
    (void)fd;(void)address;(void)length;(void)flags;
    errno=(calls++==0 || repeat)?injected_error:EAGAIN;
    return -1;
}
#define accept4 injected_accept4
#define event_loop_run accept_test_event_loop_run
#include "../src/event_loop.c"
#undef accept4
int main(void)
{
    int errors[]={ECONNABORTED,EPROTO,ENETDOWN,ENOPROTOOPT,EHOSTDOWN,ENONET,
                  EHOSTUNREACH,EOPNOTSUPP,ENETUNREACH,EINTR};
    event_loop loop={.config={.max_connections=1}};
    for(size_t i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
        injected_error=errors[i];calls=repeat=0;
        assert(!accept_connections(&loop) && calls==2);
    }
    /* Persistent transient failure consumes the budget, never a hot inner loop. */
    injected_error=ECONNABORTED;calls=0;repeat=1;
    loop.shutting=1; /* No listener registration is required for this fake loop. */
    assert(!accept_connections(&loop) && calls==ACCEPT_BUDGET);
    injected_error=EMFILE;calls=0;
    assert(accept_connections(&loop)<0 && calls==1);
    puts("Accept recovery: peer/network errors retry with fairness; fd exhaustion is fatal");
}
#else
#include <stdio.h>
int main(void) {puts("SKIP: accept4 error recovery requires Linux");}
#endif
