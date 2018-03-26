#ifndef _CONFIG_H_
#define _CONFIG_H_

#include <stdint.h>
#include <stdbool.h>

struct config {
  char *host;
  uint16_t port;
  uint16_t clientPort;
  int delayed;
  int restart;
};

extern struct config config;

extern int parseConfig(int,char **);

#endif
