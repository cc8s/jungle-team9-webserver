#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "check.h"
#include "response.h"

static char buf[4096];

/* ---- 가짜 write ----
   Makefile 이 이 테스트만 -Wl,--wrap=write 로 링크한다. 그러면 response.c 의 write 호출이
   __wrap_write 로 연결되고, 진짜 write 는 __real_write 로 부를 수 있다. 실제 코드는 그대로. */
ssize_t __real_write(int fd, const void *p, size_t n);
static int fake_write; /* 0: 진짜 write 그대로, 1: 부분 쓰기·EINTR 흉내 */
static int write_calls;

ssize_t __wrap_write(int fd, const void *p, size_t n)
{
    if (!fake_write)
        return __real_write(fd, p, n);
    /* write_all 이 끝없이 도는 결함이면 테스트가 멈추지 않고 실패로 끝나게.
       상한은 작아야 한다: 3바이트 write 도 커널은 한 번마다 관리 구조체(skb) 크기만큼 버퍼를 잡아서,
       1000 번으로 두었더니 아무도 안 읽는 소켓 버퍼가 ~280 번 만에 가득 차 진짜 write 가 멈췄다 */
    if (write_calls > 100)
    {
        errno = EIO;
        return -1;
    }
    if (write_calls++ % 2 == 0)
    { /* 한 번 걸러: 아무것도 못 쓰고 시그널에 끊긴 척 */
        errno = EINTR;
        return -1;
    }
    return __real_write(fd, p, n < 3 ? n : 3); /* 나머지: 최대 3바이트만 쓴 척 (부분 쓰기) */
}

/* 응답 r 에서 "Content-Length: N" 의 N */
static long content_length_of(const char *r)
{
    const char *p = strstr(r, "Content-Length: ");
    return p ? strtol(p + 16, NULL, 10) : -1;
}

int main(void)
{
    int n;

    /* write_all 이 끝나지 않는 결함이면 10초 뒤 SIGALRM 기본 동작으로 종료 → make test 가 멈추지 않고 실패 */
    alarm(10);

    /* ---- 상태 문구 ---- */
    CHECK(strcmp(status_text(200), "OK") == 0);
    CHECK(strcmp(status_text(404), "Not Found") == 0);
    CHECK(strcmp(status_text(505), "HTTP Version Not Supported") == 0);
    CHECK(status_text(299) == NULL);

    /* ---- 헤더 ---- */
    n = build_header(buf, sizeof(buf), 200, "text/plain", 5, 0, NULL);
    CHECK(n > 0 && (size_t)n == strlen(buf));
    CHECK(strncmp(buf, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(strstr(buf, "\r\nDate: ") != NULL && strstr(buf, " GMT\r\n") != NULL);
    CHECK(strstr(buf, "\r\nContent-Type: text/plain\r\n") != NULL);
    CHECK(strstr(buf, "\r\nContent-Length: 5\r\n") != NULL);
    CHECK(strstr(buf, "\r\nConnection: close\r\n") != NULL);
    CHECK(strstr(buf, "\r\nX-Content-Type-Options: nosniff\r\n") != NULL);
    CHECK(n >= 4 && memcmp(buf + n - 4, "\r\n\r\n", 4) == 0); /* 빈 줄로 끝남, 그 전엔 빈 줄 없음 */
    CHECK(strstr(buf, "\r\n\r\n") == buf + n - 4);

    n = build_header(buf, sizeof(buf), 200, NULL, 0, 1, "Allow: GET\r\n");
    CHECK(n > 0 && strstr(buf, "\r\nContent-Type:") == NULL); /* X-Content-Type-Options 와 구분 */
    CHECK(strstr(buf, "\r\nConnection: keep-alive\r\n") != NULL);
    CHECK(strstr(buf, "\r\nAllow: GET\r\n\r\n") != NULL);

    CHECK(build_header(buf, sizeof(buf), 299, NULL, 0, 0, NULL) == -1); /* 모르는 코드 */
    CHECK(build_header(buf, 20, 200, NULL, 0, 0, NULL) == -1);          /* 버퍼 모자람 */
    CHECK(build_header(buf, 0, 200, NULL, 0, 0, NULL) == -1);

    /* ---- 오류 응답 ---- */
    n = build_error(buf, sizeof(buf), 404, 0);
    CHECK(n > 0);
    buf[n] = '\0';
    CHECK(strncmp(buf, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    CHECK(strstr(buf, "Content-Type: text/html; charset=utf-8\r\n") != NULL);
    CHECK(strstr(buf, "Connection: close\r\n") != NULL);
    const char *body = strstr(buf, "\r\n\r\n") + 4;
    CHECK(content_length_of(buf) == (long)strlen(body)); /* 적은 길이 = 실제 본문 길이 */
    CHECK(strstr(body, "404 Not Found") != NULL);

    int full = n;
    long body_len = (long)strlen(body); /* buf 를 다시 쓰기 전에 길이를 저장 */
    n = build_error(buf, sizeof(buf), 404, 1); /* HEAD: 본문 없음, 길이 헤더는 같음 */
    buf[n] = '\0';
    CHECK(n < full && memcmp(buf + n - 4, "\r\n\r\n", 4) == 0);
    CHECK(content_length_of(buf) == body_len);

    n = build_error(buf, sizeof(buf), 405, 0);
    buf[n] = '\0';
    CHECK(strstr(buf, "\r\nAllow: GET, HEAD\r\n") != NULL);

    CHECK(build_error(buf, sizeof(buf), 299, 0) == -1);
    CHECK(build_error(buf, 200, 404, 0) == -1); /* 헤더는 들어가도 본문까지는 안 들어감 */

    /* ---- write_all: 부분 쓰기·EINTR (가짜 write 로 매번 똑같이 재현) ----
       fake_write = 1 이면 __wrap_write 가 "한 번 걸러 EINTR, 나머지는 최대 3바이트만" 쓴다. */
    const char msg[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    int sv[2];
    char got[sizeof(msg)];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    fake_write = 1;
    write_calls = 0;
    CHECK(write_all(sv[0], msg, sizeof(msg) - 1) == 0);
    fake_write = 0;
    /* 3바이트씩이면 최소 ceil(len/3) 번, EINTR 까지 그 두 배 가까이 불렸어야 한다 */
    CHECK(write_calls >= 2 * ((int)(sizeof(msg) - 1) / 3));
    /* MSG_DONTWAIT: 아무것도 안 써졌으면 기다리지 말고 바로 실패 */
    CHECK(recv(sv[1], got, sizeof(got), MSG_DONTWAIT) == (ssize_t)(sizeof(msg) - 1));
    CHECK(memcmp(got, msg, sizeof(msg) - 1) == 0); /* 순서·내용 그대로 */
    close(sv[0]);
    close(sv[1]);

    /* ---- write_all: 진짜 소켓으로 소켓 버퍼(수백 KB)보다 큰 1MB 가 다 도착하나 ---- */
    size_t total = 1024 * 1024;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid_t pid = fork();
    if (pid == 0)
    { /* 자식: 받는 쪽. 받은 바이트 수와 내용이 맞으면 0 으로 종료 */
        close(sv[0]);
        char r[4096];
        size_t cnt = 0;
        int bad = 0;
        ssize_t k;
        while ((k = read(sv[1], r, sizeof(r))) > 0)
        {
            for (ssize_t i = 0; i < k; i++)
                bad |= r[i] != (char)((cnt + i) % 251);
            cnt += k;
        }
        _exit(cnt == total && !bad ? 0 : 1);
    }
    close(sv[1]);
    char *big = malloc(total);
    for (size_t i = 0; i < total; i++)
        big[i] = (char)(i % 251);
    CHECK(write_all(sv[0], big, total) == 0);
    close(sv[0]); /* 닫아야 자식이 EOF 를 받는다 */
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    free(big);

    /* 닫힌 fd 에 쓰면 -1 */
    CHECK(write_all(-1, "x", 1) == -1);

    CHECK_DONE();
}
