#ifndef PREFORK_H
#define PREFORK_H

/* worker 프로세스가 할 일. 반환값이 그 프로세스의 종료 코드 */
typedef int (*work_fn)(void *arg);

/* worker n 개를 fork 해 각자 work(arg) 를 돌린다 (D1 ⑤).
 * - worker 가 죽으면 다시 만든다 (1초 안에 죽으면 1초 쉬고: 바로 죽는 버그로 fork 가 폭주하지 않게)
 * - SIGINT·SIGTERM (install_signals 로 미리 등록) 을 받으면 worker 들에게 SIGTERM 을 보내고 전부 회수
 * 부모는 worker 가 무슨 일을 하는지 모른다 → ⑥ 에서 work 만 epoll 판으로 바꾼다.
 * 정상 종료 0, 처음 fork 실패 -1 */
int prefork_run(int n, work_fn work, void *arg);

#endif
