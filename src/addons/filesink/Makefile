CC ?= cc
CFLAGS ?= -O2
LDFLAGS ?= -pthread
PH_CFLAGS := -std=c11 -Wall -Wextra -fPIC -pthread

INCS = -I../../../include
NAME := filesink
SO := ph-lib$(NAME).so

.PHONY: all clean

SRCS_SO := src/$(NAME).c \
../../../src/common.c \
../../../src/common/ctrlmsg.c \
../../../src/common/ph_shm.c \
../../../src/common/ph_subs.c \
../../../src/common/ph_ring.c

all: $(SO)

$(SO): $(SRCS_SO)
	$(CC) $(PH_CFLAGS) $(CFLAGS) $(INCS) -shared -o $@ $^ $(LDFLAGS) -ldl

clean:
	rm -f $(SO)
