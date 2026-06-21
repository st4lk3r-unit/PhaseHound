CC      ?= cc
CFLAGS ?= -O2
LDFLAGS ?= -pthread -Wl,-z,defs
PH_CFLAGS := -std=c11 -Wall -Wextra -fPIC -pthread

ADDON   := dummy
SO      := ph-lib$(ADDON).so

ROOT       := ../../../
INC_DIRS   := $(ROOT)include $(ROOT)src/common
INCS       := $(addprefix -I,$(INC_DIRS))

SRCS := src/$(ADDON).c \
        $(ROOT)src/common.c \
        $(ROOT)src/common/ctrlmsg.c \
        $(ROOT)src/common/ph_shm.c \
        $(ROOT)src/common/ph_subs.c

.PHONY: all clean

all: $(SO)

$(SO): $(SRCS)
	$(CC) $(PH_CFLAGS) $(CFLAGS) $(INCS) -shared -o $@ $^ $(LDFLAGS) -ldl

clean:
	rm -f $(SO)
