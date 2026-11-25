#ifndef SIGNALS_H
#define SIGNALS_H

/* SIGINT·SIGTERM 은 "그만하라" 표시만 세우고, SIGPIPE 는 무시한다. 성공 0, 실패 -1.
 * SA_RESTART 를 켜지 않으므로, 시그널이 오면 accept 처럼 기다리던 호출이 EINTR 로 돌아온다
 * → 호출한 쪽이 stop_requested() 를 보고 루프를 빠져나가면 된다. */
int install_signals(void);

/* SIGINT·SIGTERM 을 받았는가 */
int stop_requested(void);

#endif
