#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "check.h"
#include "prefork.h"
#include "signals.h"

/* 테스트용 worker 의 일: 자기 pid 를 파이프로 알리고, 종료 시그널까지 잔다 */
static int report_and_wait(void *arg)
{
    int fd = *(int *)arg;
    pid_t me = getpid();
    if (write(fd, &me, sizeof(me)) != sizeof(me))
        return 1;
    while (!stop_requested())
        pause(); /* 시그널이 오면 깨어난다 */
    return 0;
}

/* 파이프에서 pid 하나. 5초 안에 안 오면 -1 (결함이 있어도 멈추지 않고 FAIL 줄이 찍히게) */
static pid_t read_pid(int fd)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    pid_t pid;
    if (poll(&p, 1, 5000) != 1)
        return -1;
    return read(fd, &pid, sizeof(pid)) == sizeof(pid) ? pid : -1;
}

/* 5초 안에 끝나면 waitpid 결과, 아니면 0 */
static pid_t wait_upto_5s(pid_t pid, int *st)
{
    for (int i = 0; i < 50; i++)
    {
        pid_t r = waitpid(pid, st, WNOHANG);
        if (r != 0)
            return r;
        usleep(100 * 1000);
    }
    return 0;
}

/* 관리자와 그 worker 들은 한 프로세스 그룹. 결함으로 정리가 안 돼도 테스트가 그룹째 지운다 */
static pid_t mgr;

static void kill_group(void)
{
    if (mgr > 0)
        kill(-mgr, SIGKILL); /* 음수 pid = 그 프로세스 그룹 전체 */
}

static void on_alarm(int sig)
{
    (void)sig;
    kill_group(); /* 테스트 본체만 죽으면 관리자·worker 가 고아로 남는다 */
    _exit(1);
}

int main(void)
{
    int pfd[2];
    pid_t w[3];

    signal(SIGALRM, on_alarm);
    alarm(30); /* 그래도 어딘가 멈추면 마지막 안전장치 */
    CHECK(pipe(pfd) == 0);

    /* 관리자(부모 역할) 프로세스: 실제 서버의 main 처럼 시그널 등록 후 prefork_run */
    mgr = fork();
    if (mgr == 0)
    {
        setpgid(0, 0); /* 자기 pid 로 새 프로세스 그룹 → worker 들도 이 그룹을 물려받음 */
        close(pfd[0]);
        install_signals();
        exit(prefork_run(3, report_and_wait, &pfd[1]) == 0 ? 0 : 1);
    }
    close(pfd[1]);
    setpgid(mgr, mgr); /* 자식이 setpgid 하기 전에 아래 kill(-mgr) 이 불려도 되게 부모도 한 번 (경쟁 방지 관례) */

    /* 1. worker 3개가 떴다 */
    for (int i = 0; i < 3; i++)
        w[i] = read_pid(pfd[0]);
    CHECK(w[0] > 0 && w[1] > 0 && w[2] > 0);
    CHECK(w[0] != w[1] && w[1] != w[2] && w[0] != w[2]);

    /* 2. worker 하나가 죽으면 (segfault 처럼 SIGKILL) 관리자가 새로 만든다.
     *    방금 태어난 worker 라 "1초 안에 죽음" → 1초 쉬고 만든다 */
    CHECK(kill(w[0], SIGKILL) == 0);
    pid_t fresh = read_pid(pfd[0]);
    CHECK(fresh > 0 && fresh != w[0] && fresh != w[1] && fresh != w[2]);
    CHECK(kill(w[1], 0) == 0 && kill(w[2], 0) == 0); /* 나머지는 그대로 살아 있음 (격리) */

    /* 3. 관리자에게 SIGTERM → worker 전부 회수하고 exit 0 */
    CHECK(kill(mgr, SIGTERM) == 0);
    int st = 0;
    CHECK(wait_upto_5s(mgr, &st) == mgr);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* 관리자가 회수했으므로 worker 는 좀비로도 남지 않는다 (pid 가 아예 없음) */
    pid_t all[] = {w[1], w[2], fresh};
    for (int i = 0; i < 3; i++)
    {
        errno = 0;
        CHECK(kill(all[i], 0) == -1 && errno == ESRCH);
    }

    kill_group(); /* 정상이면 이미 아무도 없음. 결함으로 남은 프로세스가 있으면 여기서 정리 */
    waitpid(mgr, NULL, WNOHANG);
    CHECK_DONE();
}
