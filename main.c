#include "slog.h"
#include "config.h"

#include "rtlmux.h"

#include <pthread.h>
#include <string.h>

unsigned char timeToExit = 0;

int main(int argc, char **argv) {
  pthread_t threadServer;
  int result;
  
  parseConfig(argc, argv);
  slog_init(NULL, NULL, LOG_EXTRA, LOG_DEBUG, 1);
  
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
