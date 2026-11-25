#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "worker.h"

static char buf[CONN_BUF_SIZE];
static struct request req;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* 자식이 fd 에 text 를 gap_ms 간격으로 step 바이트씩 보낸 뒤 (close_after 면) 닫는다 */
static pid_t sender(int fd, const char *text, int step, int gap_ms, int close_after)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        size_t n = strlen(text);
        for (size_t i = 0; i < n; i += step)
        {
            if (write(fd, text + i, (size_t)step < n - i ? (size_t)step : n - i) < 0)
                _exit(0); /* 받는 쪽이 이미 닫음 */
            usleep(gap_ms * 1000);
        }
        if (!close_after)
            pause(); /* 닫지 않고 버팀 (부모가 끝나면 kill) */
        _exit(0);
    }
    return pid;
}

/* 소켓 쌍을 만들고 자식 sender 를 붙여 read_request 결과와 걸린 시간을 잰다 */
static enum read_result run(const char *text, int step, int gap_ms, int close_after, int timeout_ms, long *ms)
{
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    pid_t pid = sender(sv[1], text, step, gap_ms, close_after);
    close(sv[1]); /* 부모 쪽 사본을 닫아야 자식이 닫을 때 EOF 가 온다 */
    long t0 = now_ms();
    enum read_result r = read_request(sv[0], buf, sizeof(buf), timeout_ms, &req);
    *ms = now_ms() - t0;
    close(sv[0]);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return r;
}

int main(void)
{
    const char *get = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
    long ms;

    alarm(20); /* 마감이 안 지켜지는 결함이면 멈추지 말고 실패로 끝나게 */

    /* 한 번에 다 옴 */
    CHECK(run(get, 1000, 0, 0, 1000, &ms) == READ_OK);
    CHECK(strcmp(req.uri, "/") == 0);

    /* 여러 조각이지만 시간 안에 다 옴 (4바이트씩 10ms 간격 → 약 70ms) */
    CHECK(run(get, 4, 10, 0, 2000, &ms) == READ_OK);

    /* 반만 보내고 멈춤 → 마감(300ms)에 타임아웃 */
    CHECK(run("GET / HTTP/1.1\r\nHo", 1000, 0, 0, 300, &ms) == READ_TIMEOUT);
    CHECK(ms >= 250 && ms < 1000);

    /* ★ 찔끔 보내기 (slowloris): 1바이트씩 100ms 간격 → 다 보내려면 약 2.7초.
     *   read 마다 시간을 새로 세는 방식(SO_RCVTIMEO)이면 100ms < 500ms 라 끝까지 기다리지만,
     *   요청 전체 마감이면 약 500ms 에 끊어야 한다 */
    CHECK(run(get, 1, 100, 0, 500, &ms) == READ_TIMEOUT);
    CHECK(ms >= 450 && ms < 1000);

    /* 요청 도중 손님이 닫음 */
    CHECK(run("GET / HTTP/1.1\r\nHo", 1000, 0, 1, 2000, &ms) == READ_CLOSED);
    CHECK(ms < 1000); /* 닫히면 마감까지 기다리지 않고 바로 */

    /* 잘못된 요청 → 파서의 상태 코드 */
    CHECK(run("GET / HTTP/2.0\r\nHost: a\r\n\r\n", 1000, 0, 0, 1000, &ms) == READ_INVALID);
    CHECK(req.status == 505);

    CHECK_DONE();
}
