#include "event_loop.h"
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static void usage(const char *name)
{
    fprintf(stderr,"Usage: %s [--worker 4] [--queue 64] [--max 128] [--buffer 32]\n"
                   "  --worker 1..1024, --queue/--max 1..65536, --buffer 1..1048576 MiB\n"
                   "  Linux epoll; port 8000; static root ./www\n",name);
}
static int number(const char *text,size_t limit,size_t *out)
{
    if(!text || !*text) return -1;
    for(const char *p=text;*p;p++) if(*p<'0' || *p>'9') return -1;
    errno=0;char *end;unsigned long long value=strtoull(text,&end,10);
    if(errno || *end || !value || value>limit || value>SIZE_MAX) return -1;
    *out=(size_t)value;return 0;
}
int main(int argc,char **argv)
{
    server_config config={.workers=4,.queue=64,.max_connections=128,.buffer_budget=32};
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--help")) {usage(argv[0]);return EXIT_SUCCESS;}
        size_t *value=NULL,limit=65536;
        if(!strcmp(argv[i],"--worker")) {value=&config.workers;limit=1024;}
        else if(!strcmp(argv[i],"--queue")) value=&config.queue;
        else if(!strcmp(argv[i],"--max")) value=&config.max_connections;
        else if(!strcmp(argv[i],"--buffer")) {value=&config.buffer_budget;limit=1048576;}
        if(!value || i+1==argc || number(argv[++i],limit,value)) {usage(argv[0]);return EXIT_FAILURE;}
    }
    if(config.buffer_budget>SIZE_MAX/(1024*1024)) {usage(argv[0]);return EXIT_FAILURE;}
    config.buffer_budget*=1024*1024;
    if(signal(SIGPIPE,SIG_IGN)==SIG_ERR) {perror("SIGPIPE");return EXIT_FAILURE;}
    return event_loop_run(&config)?EXIT_FAILURE:EXIT_SUCCESS;
}
