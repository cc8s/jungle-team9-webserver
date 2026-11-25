#include "static.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mime.h"
#include "path.h"
#include "response.h"

int static_method_status(const char *method)
{
    static const char *known[] = {"POST", "PUT", "DELETE", "PATCH", "OPTIONS", "CONNECT", "TRACE"};

    if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0)
        return 0;
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (strcmp(method, known[i]) == 0) /* 메서드는 대소문자 구분 (RFC 9110) */
            return 405;
    return 501;
}

/* 오류 응답을 보내고 그 상태 코드를 돌려준다 */
static int send_error(int fd, int status, int head_only)
{
    char out[1024];
    int n = build_error(out, sizeof(out), status, head_only);
    if (n < 0 || write_all(fd, out, (size_t)n) < 0)
        return -1;
    return status;
}

int serve_static(int fd, const char *root_real, const struct request *req)
{
    int head_only = strcmp(req->method, "HEAD") == 0;
    char path[PATH_MAX], hdr[512], chunk[64 * 1024];
    struct stat st;

    int st_code = static_method_status(req->method);
    if (st_code != 0)
        return send_error(fd, st_code, head_only);
    st_code = resolve_path(root_real, req->uri, path, sizeof(path));
    if (st_code != 0)
        return send_error(fd, st_code, head_only);

    /* resolve 와 open 사이에 파일이 바뀔 수 있으니 열고 나서 fstat 으로 크기·종류를 다시 본다 */
    int file = open(path, O_RDONLY);
    if (file < 0)
        return send_error(fd, errno == EACCES ? 403 : 404, head_only);
    if (fstat(file, &st) < 0 || !S_ISREG(st.st_mode))
    {
        close(file);
        return send_error(fd, 403, head_only);
    }

    int n = build_header(hdr, sizeof(hdr), 200, mime_type(path), (size_t)st.st_size, 0, NULL);
    if (n < 0)
    {
        close(file);
        return send_error(fd, 500, head_only);
    }
    if (write_all(fd, hdr, (size_t)n) < 0)
    {
        close(file);
        return -1;
    }

    /* 본문: 파일 전체를 메모리에 올리지 않고 64KB 씩 읽어 보낸다 (큰 파일도 메모리 일정).
       헤더를 이미 보냈으니 여기서 실패하면 오류 응답을 못 보낸다 → 연결을 끊어 손님이 알게 */
    off_t left = head_only ? 0 : st.st_size;
    while (left > 0)
    {
        ssize_t r = read(file, chunk, sizeof(chunk));
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0 || write_all(fd, chunk, (size_t)r) < 0)
        {
            close(file);
            return -1;
        }
        left -= r;
    }
    close(file);
    return 200;
}
