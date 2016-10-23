CC=gcc
CFLAGS=-Wall -I/usr/local/include -O3

LIBS=-pthread `pkg-config libevent --libs-only-L` -levent -levent_pthreads

ifeq ($(shell uname -m),armv7l)
  CFLAGS+=-O3 -mfpu=neon-vfpv4 -mfloat-abi=hard -march=armv7-a -ffast-math -funsafe-math-optimizations
else
  CFLAGS+=-O3
endif

ifneq ($(shell uname),Darwin)
  LIBS+=-lrt
endif

ifeq ($(shell uname),Darwin)
  CFLAGS+=-glldb
else
  CFLAGS+=-ggdb3
endif

SRC=slog.c rtlmux.c config.c cmdline.c main.c
OBJS=slog.o rtlmux.o config.o cmdline.o main.o

all: cmdline.c rtlmux

clean:
	rm -f rtlmux $(OBJS)

depend:
	makedepend -- $(CFLAGS) -- $(SRC)

cmdline.h: options.ggo
	gengetopt -C -i $< -f cmdline.c

cmdline.c: cmdline.h

rtlmux: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

# DO NOT DELETE
