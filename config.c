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
  if(args->port_arg < 1 || args->port_arg > UINT16_MAX) {
    fprintf(stderr, "rtl_tcp port must be between 1 and %u.\n", UINT16_MAX);
    return 0;
  }
  if(args->listen_arg < 1 || args->listen_arg >= UINT16_MAX) {
    fprintf(stderr, "Listening port must be between 1 and %u to reserve the following port for HTTP status.\n", UINT16_MAX - 1);
    return 0;
  }
  if(args->host_arg == NULL || args->host_arg[0] == '\0') {
    fprintf(stderr, "rtl_tcp host address cannot be empty.\n");
    return 0;
  }

  config.host = args->host_arg;
  config.port = (uint16_t)args->port_arg;
  config.clientPort = (uint16_t)args->listen_arg;
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
  
  if(!convertConfig(&args))
    exit(4);

  return 1;
}
