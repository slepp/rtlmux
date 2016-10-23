#ifndef _SERVER_H_
#define _SERVER_H_

extern volatile unsigned char timeToExit;

extern void *serverThread(void *);

struct command {
        unsigned char cmd;
        unsigned int param;
}__attribute__((packed));

#endif
