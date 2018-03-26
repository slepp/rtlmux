#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include <getopt.h>

#include "cmdline.h"
#include "config.h"

#include "slog.h"

struct config config;

static struct gengetopt_args_info args;

int convertConfig(struct gengetopt_args_info *args) {
  config.host = args->host_arg;
  config.port = args->port_arg;
  config.clientPort = args->listen_arg;
  config.delayed = args->delayed_flag;
  config.restart = args->restart_flag;

  return 1;
}

int parseConfig(int argc, char **argv) {
  int cr;
  struct cmdline_c_params params;
  cmdline_c_params_init(&params);
  params.initialize = 1;
  params.override = 0;
  params.check_required = 0;
  params.check_ambiguity = 0;
  if((cr = cmdline_c_ext(argc, argv, &args, &params)) != 0) {
    exit(2);
  }
  
  if(cmdline_c_required(&args, argv[0]) != 0) {
    printf("Please try again.");
    exit(4);
  }
  
  return convertConfig(&args);
}
