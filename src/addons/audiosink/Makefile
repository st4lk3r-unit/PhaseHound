CC ?= cc
CFLAGS ?= -O2
LDFLAGS ?= -pthread
PH_CFLAGS := -std=c11 -Wall -Wextra -fPIC -pthread
INCS = -I../../../include

NAME := audiosink
SO := ph-lib$(NAME).so

HAVE_ALSA := $(shell pkg-config --exists alsa >/dev/null 2>&1 && echo 1 || echo 0)
ALSA_CFLAGS := $(shell pkg-config --cflags alsa 2>/dev/null)
ALSA_LIBS := $(shell pkg-config --libs alsa 2>/dev/null || echo -lasound)

.PHONY: all clean

SRCS_SO := src/$(NAME).c \
src/audiosink_alsa.c \
src/audiosink_ring.c \
../../../src/common.c \
../../../src/common/ctrlmsg.c \
../../../src/common/ph_shm.c \
../../../src/common/ph_subs.c \
../../../src/common/ph_ring.c

ifeq ($(HAVE_ALSA),1)
all: $(SO)
$(SO): $(SRCS_SO)
	$(CC) $(PH_CFLAGS) $(CFLAGS) $(ALSA_CFLAGS) $(INCS) -shared -o $@ $^ $(LDFLAGS) $(ALSA_LIBS) -ldl
else
all:
	@echo "[audiosink] skipping: missing ALSA development package (pkg-config alsa). Set REQUIRE_DEPS=1 to fail."
	@test "$(REQUIRE_DEPS)" != "1"
endif

clean:
	rm -f $(SO)
