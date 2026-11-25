#include "signals.h"

#include <signal.h>
#include <string.h>

/* 핸들러와 main 이 같이 보는 값: volatile (매번 메모리에서 다시 읽게) + sig_atomic_t (한 번에 읽고 쓰임) */
static volatile sig_atomic_t stop_flag;

/* 핸들러 안에서는 async-signal-safe 함수만 쓸 수 있다 (printf·malloc·free 는 안 됨).
 * 그래서 표시만 세우고, 정리와 출력은 main 이 루프를 빠져나와서 한다. */
static void on_stop(int sig)
{
    (void)sig;
    stop_flag = 1;
}

int install_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* SA_RESTART 없음: accept 가 EINTR 로 깨어나야 종료 표시를 볼 수 있다 */
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;

    /* 끊긴 손님에게 쓰면 커널이 SIGPIPE 를 보내고, 기본 동작은 프로세스 종료.
     * 무시하면 write 가 -1 (EPIPE) 을 돌려주므로 그 연결만 정리하고 서버는 계속 산다 */
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) < 0)
        return -1;
    return 0;
}

int stop_requested(void)
{
    return stop_flag;
}
