#include "slog.h"
#include "config.h"

#include "rtlmux.h"

int main(int argc, char **argv) {
  parseConfig(argc, argv);
  slog_init(NULL, NULL, LOG_EXTRA, LOG_DEBUG, 1);
  serverThread(0);
}
