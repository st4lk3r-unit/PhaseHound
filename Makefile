# Top-level Makefile
CC ?= cc
CFLAGS ?= -O2
LDFLAGS ?=
PH_CFLAGS := -std=c11 -Wall -Wextra -pthread -fPIC
PH_LDFLAGS := -ldl -pthread

PREFIX ?= /usr/local

INCS = -Iinclude

CORE_SRCS = src/core.c src/common.c
CORE_OBJS = $(CORE_SRCS:.c=.o)
CORE_BIN  = ph-core

CLI_SRCS  = tools/cli.c src/common.c
CLI_OBJS  = $(CLI_SRCS:.c=.o)
CLI_BIN   = ph-cli

GIT_SHA := $(shell git rev-parse --short=7 HEAD 2>/dev/null || echo unknown)
CFLAGS  += -DPH_GIT_SHA=\"$(GIT_SHA)\"

WATERFALL_BIN  := ph-waterfall
WATERFALL_SRCS := tools/waterfall.c src/common.c src/dsp/ph_dsp.c \
                  src/common/ph_ring.c src/common/ph_shm.c
WF_CFLAGS := $(shell pkg-config --cflags glfw3 2>/dev/null)
WF_LIBS   := $(shell pkg-config --libs   glfw3 2>/dev/null) -lGL -lm

.PHONY: all clean addons install waterfall

all: $(CORE_BIN) $(CLI_BIN) addons

$(CORE_BIN): $(CORE_OBJS)
	$(CC) $(PH_CFLAGS) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(PH_LDFLAGS)

$(CLI_BIN): $(CLI_OBJS)
	$(CC) $(PH_CFLAGS) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(PH_LDFLAGS)

%.o: %.c
	$(CC) $(PH_CFLAGS) $(CFLAGS) $(INCS) -c $< -o $@

addons:
	@set -e; for d in $(wildcard src/addons/*); do \
	  if [ -f $$d/Makefile ]; then echo "[addons] building $$d"; $(MAKE) -C $$d; fi; \
	done

clean:
	rm -f $(CORE_OBJS) $(CLI_OBJS) $(CORE_BIN) $(CLI_BIN) $(WATERFALL_BIN)
	@find src tools -type f -name '*.o' -delete
	@for d in $(wildcard src/addons/*); do \
	  if [ -f $$d/Makefile ]; then echo "[addons] cleaning $$d"; $(MAKE) -C $$d clean; fi; \
	done

waterfall: $(WATERFALL_SRCS)
	$(CC) $(PH_CFLAGS) $(CFLAGS) $(INCS) $(WF_CFLAGS) $(WATERFALL_SRCS) \
	  -o $(WATERFALL_BIN) $(LDFLAGS) $(PH_LDFLAGS) $(WF_LIBS)
	@echo "[waterfall] built $(WATERFALL_BIN)"

install:
	install -m755 $(CORE_BIN) $(PREFIX)/bin/$(CORE_BIN)
	install -m755 $(CLI_BIN)  $(PREFIX)/bin/$(CLI_BIN)
