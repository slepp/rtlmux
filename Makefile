CC ?= cc
PKG_CONFIG ?= pkg-config
PYTHON ?= python3
GENGETOPT ?= gengetopt

PREFIX ?= /usr/local
DESTDIR ?=

CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libevent_pthreads)
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -Wformat=2 -pthread
LDFLAGS ?=
LDLIBS += -pthread $(shell $(PKG_CONFIG) --libs libevent_pthreads)

ifeq ($(STATIC),1)
  LDFLAGS += -static
  LDLIBS := -pthread $(shell $(PKG_CONFIG) --static --libs libevent_pthreads)
endif

SRC = slog.c rtlmux.c config.c cmdline.c main.c
OBJS = $(SRC:.c=.o)
DEPS = $(OBJS:.o=.d)

.PHONY: all clean generate install static test

all: rtlmux

rtlmux: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

cmdline.o: CFLAGS += -Wno-unused-but-set-variable

generate: options.ggo
	$(GENGETOPT) -C -i $< -f cmdline.c

static:
	$(MAKE) clean
	$(MAKE) STATIC=1 all

test: rtlmux
	$(PYTHON) tests/integration.py ./rtlmux

install: rtlmux
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 rtlmux $(DESTDIR)$(PREFIX)/bin/rtlmux

clean:
	rm -f rtlmux $(OBJS) $(DEPS)

-include $(DEPS)
