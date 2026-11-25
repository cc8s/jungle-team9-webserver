#include "prefork.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "signals.h"

static pid_t spawn(work_fn work, void *arg)
{
    fflush(NULL); /* 비우지 않으면 부모 stdout 버퍼가 자식에게 복사돼 같은 줄이 두 번 찍힌다 */
    pid_t pid = fork();
    if (pid == 0)
        exit(work(arg)); /* exit: 자식도 stdout 을 비우고 LeakSanitizer 검사를 거친다 */
    return pid;
}

static void report(pid_t pid, int status)
{
    if (WIFSIGNALED(status))
        printf("worker %d killed by signal %d\n", (int)pid, WTERMSIG(status));
    else
        printf("worker %d exited with %d\n", (int)pid, WEXITSTATUS(status));
}

int prefork_run(int n, work_fn work, void *arg)
{
    pid_t *pids = calloc((size_t)n, sizeof(pid_t));
    time_t *born = calloc((size_t)n, sizeof(time_t));
    int rc = 0;

    if (pids == NULL || born == NULL)
    {
        rc = -1;
        goto out;
    }
    for (int i = 0; i < n; i++)
    {
        pids[i] = spawn(work, arg);
        born[i] = time(NULL);
        if (pids[i] < 0)
        {
            perror("fork");
            rc = -1;
            goto shutdown; /* 이미 만든 worker 는 정리하고 실패 */
        }
    }
    printf("started %d workers\n", n);
    fflush(stdout);

    /* 부모의 일: worker 가 죽기를 기다렸다가 다시 만든다. 종료 시그널이면 waitpid 가 EINTR 로 깨어남 */
    while (!stop_requested())
    {
        int status;
        pid_t dead = waitpid(-1, &status, 0);
        if (dead < 0)
        {
            if (errno == EINTR)
                continue;
            break; /* 기다릴 자식이 없음 (ECHILD) */
        }
        for (int i = 0; i < n; i++)
        {
            if (pids[i] != dead)
                continue;
            report(dead, status);
            pids[i] = 0;
            if (stop_requested()) /* Ctrl-C 는 worker 에게도 바로 가므로, 이 죽음은 종료 과정일 수 있다 */
                break;
            if (time(NULL) - born[i] < 1)
                sleep(1); /* 바로 죽는 worker 를 쉬지 않고 다시 만들면 fork 폭주 */
            pids[i] = spawn(work, arg);
            born[i] = time(NULL);
            if (pids[i] < 0)
            {
                perror("fork");
                pids[i] = 0;
            }
            else
                printf("respawned worker %d\n", (int)pids[i]);
            fflush(stdout);
            break;
        }
    }

shutdown:
    /* 남은 worker 에게 종료를 알리고, 전부 끝날 때까지 회수한다 (좀비 방지) */
    for (int i = 0; i < n; i++)
        if (pids[i] > 0)
            kill(pids[i], SIGTERM);
    for (int i = 0; i < n; i++)
        if (pids[i] > 0)
            while (waitpid(pids[i], NULL, 0) < 0 && errno == EINTR)
                ;
out:
    free(pids);
    free(born);
    return rc;
}
