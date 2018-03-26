#include "slog.h"
#include "config.h"

#include "rtlmux.h"

#include <signal.h>

volatile unsigned char timeToExit = 0;

void signalExit(int sig) {
  timeToExit = 1;
}

int main(int argc, char **argv) {
  pthread_t threadServer;
  
  parseConfig(argc, argv);
  slog_init(NULL, NULL, LOG_EXTRA, LOG_DEBUG, 1);

  struct sigaction sigact;
  sigact.sa_handler = signalExit;
  sigact.sa_flags = 0;
  sigaction(SIGTERM, &sigact, NULL);
  sigaction(SIGINT, &sigact, NULL);
restart:
  pthread_create(&threadServer, NULL, serverThread, NULL);

  pthread_join(threadServer, NULL);

  if (timeToExit == 2) {
    slog(LOG_INFO, SLOG_INFO, "Restarting.");
    timeToExit = 0;
    goto restart;
  }
}
