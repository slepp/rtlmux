#include "slog.h"
#include "config.h"

#include "rtlmux.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>

volatile sig_atomic_t timeToExit = 0;

void signalExit(int sig) {
  (void)sig;
  timeToExit = 1;
}

int main(int argc, char **argv) {
  pthread_t threadServer;
  int result;
  
  parseConfig(argc, argv);
  slog_init(NULL, NULL, LOG_EXTRA, LOG_DEBUG, 1);

  struct sigaction sigact;
  memset(&sigact, 0, sizeof(sigact));
  sigact.sa_handler = signalExit;
  sigact.sa_flags = 0;
  sigemptyset(&sigact.sa_mask);
  if(sigaction(SIGTERM, &sigact, NULL) != 0 || sigaction(SIGINT, &sigact, NULL) != 0) {
    slog(LOG_FATAL, SLOG_FATAL, "Could not configure signal handlers: %s", strerror(errno));
    return 1;
  }
  
  do {
    result = pthread_create(&threadServer, NULL, serverThread, NULL);
    if(result != 0) {
      slog(LOG_FATAL, SLOG_FATAL, "Could not start server thread: %s", strerror(result));
      return 1;
    }

    result = pthread_join(threadServer, NULL);
    if(result != 0) {
      slog(LOG_FATAL, SLOG_FATAL, "Could not join server thread: %s", strerror(result));
      return 1;
    }

    if (timeToExit == 2) {
      slog(LOG_INFO, SLOG_INFO, "Restarting.");
      timeToExit = 0;
    }
  } while (timeToExit != 1);

  return 0;
}
