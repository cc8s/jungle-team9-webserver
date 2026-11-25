#ifndef NET_H
#define NET_H

/* 손님 연결을 기다리는 대기 소켓을 연다 (CSAPP 11.4.8 open_listenfd 구조).
 * port: "8080" 처럼 숫자 문자열. "0" 이면 OS 가 빈 포트를 골라 준다.
 * 성공하면 대기 소켓 fd, 실패하면 -1. */
int open_listenfd(const char *port);

#endif
