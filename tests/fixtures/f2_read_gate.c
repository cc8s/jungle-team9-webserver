/* Integration-only syscall gate: hold reads of www/slow.bin in worker threads. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
ssize_t read(int fd,void *buffer,size_t count)
{
    ssize_t (*real_read)(int,void *,size_t)=dlsym(RTLD_NEXT,"read");
    const char *gate=getenv("F2_READ_GATE"), *entered=getenv("F2_GATE_ENTERED");
    if(gate) {
        char link[64],path[4096];
        snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
        ssize_t n=readlink(link,path,sizeof(path)-1);
        if(n>0) {
            path[n]=0;
            if(strstr(path,"/www/slow.bin")) {
                if(entered) {int marker=open(entered,O_CREAT|O_WRONLY,0600);if(marker>=0) close(marker);}
                while(access(gate,F_OK)==0) usleep(1000);
            }
        }
    }
    return real_read(fd,buffer,count);
}
