#ifndef _SERVER_H_
#define _SERVER_H_

#include <stdint.h>

extern unsigned char timeToExit;

extern void *serverThread(void *);

struct command {
        uint8_t cmd;
        uint32_t param;
}__attribute__((packed));

#endif
