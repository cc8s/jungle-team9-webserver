#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "check.h"
#include "signals.h"

int main(void)
{
    struct sigaction cur;
    int sv[2];
    char c;

    alarm(10); /* 아래 read 가 깨어나지 않는 결함이면 멈추지 말고 실패로 끝나게 */

    CHECK(install_signals() == 0);
    CHECK(stop_requested() == 0);

    /* SA_RESTART 가 꺼져 있어야 accept·read 가 EINTR 로 돌아와 종료 표시를 볼 수 있다 */
    sigaction(SIGTERM, NULL, &cur);
    CHECK((cur.sa_flags & SA_RESTART) == 0);
    sigaction(SIGPIPE, NULL, &cur);
    CHECK(cur.sa_handler == SIG_IGN);

    /* 1. 기다리던 read 가 SIGTERM 에 EINTR 로 깨어나고 표시가 선다.
     *    자식이 0.2초 뒤 부모에게 SIGTERM 을 보낸다 (그동안 부모는 아무도 안 쓰는 소켓에서 read 대기) */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid_t pid = fork();
    if (pid == 0)
    {
        usleep(200 * 1000);
        kill(getppid(), SIGTERM);
        _exit(0);
    }
    ssize_t r = read(sv[0], &c, 1);
    CHECK(r == -1 && errno == EINTR);
    CHECK(stop_requested() == 1); /* 프로세스는 죽지 않고 표시만 섰다 */
    waitpid(pid, NULL, 0);
    close(sv[0]);
    close(sv[1]);

    /* 2. SIGINT 도 같은 표시 (Ctrl-C) — 받아도 죽지 않는다 */
    CHECK(raise(SIGINT) == 0);

    /* 3. 끊긴 상대에게 쓰면: 프로세스가 죽지 않고 write 가 EPIPE 로 실패 */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    close(sv[1]);
    errno = 0;
    CHECK(write(sv[0], "x", 1) == -1 && errno == EPIPE);
    close(sv[0]);

    CHECK_DONE(); /* 여기까지 왔다 = SIGTERM·SIGINT·SIGPIPE 를 받고도 살아 있음 */
}
