#ifndef _SERVER_H_
#define _SERVER_H_

#include <signal.h>
#include <stdint.h>

extern volatile sig_atomic_t timeToExit;

extern void *serverThread(void *);

struct command {
        uint8_t cmd;
        uint32_t param;
}__attribute__((packed));

#endif
