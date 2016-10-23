#ifndef _CONFIG_H_
#define _CONFIG_H_

#include <stdint.h>
#include <stdbool.h>

struct config {
  char *host;
  uint16_t port;
  uint16_t clientPort;
};

extern struct config config;

extern int parseConfig(int,char **);

#endif
