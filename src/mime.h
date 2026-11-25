#ifndef MIME_H
#define MIME_H

/* 파일 경로의 확장자로 Content-Type 을 고른다 (대소문자 무시).
   모르는 확장자·확장자 없음은 "application/octet-stream" (브라우저가 실행하지 않고 내려받게) */
const char *mime_type(const char *path);

#endif
