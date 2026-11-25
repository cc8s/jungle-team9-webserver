#ifndef CONFIG_H
#define CONFIG_H

#define MAX_ROUTES 8
#define MAX_WORKERS 256
#define DEFAULT_TIMEOUT_SEC 5

/* 서버를 켤 때 받는 설정 (설계서 D4: -p 포트 -r 정적루트 -R 라우팅 -t 타임아웃, D1: -w worker 수) */
struct config {
    const char *port;                /* 필수. 1~65535 숫자 문자열 */
    const char *root;                /* 필수. 정적 파일 루트 폴더 */
    const char *routes[MAX_ROUTES];  /* -R 값 원문 ("/api=127.0.0.1:8001"). 해석은 프록시 모듈(#8) 몫 */
    int nroutes;
    int timeout_sec;                 /* 요청 읽기·응답 쓰기·백엔드 타임아웃 초. 기본 DEFAULT_TIMEOUT_SEC */
    int workers;                     /* worker 프로세스 수 1~MAX_WORKERS. 0 = 안 정함 (main 이 코어 수로) */
};

/* argv 를 읽어 cfg 를 채운다. 성공 0, 실패 -1 (*err 에 이유 문장).
 * 화면에 출력하지 않는 순수 함수라 단위 테스트할 수 있다. */
int parse_args(int argc, char **argv, struct config *cfg, const char **err);

#endif
