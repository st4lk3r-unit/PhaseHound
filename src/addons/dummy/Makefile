CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -fPIC -pthread
LDFLAGS ?= -pthread -Wl,-z,defs

ADDON   := dummy
SO      := ph-lib$(ADDON).so

ROOT       := ../../../
INC_DIRS   := $(ROOT)include $(ROOT)src/common
INCS       := $(addprefix -I,$(INC_DIRS))

# Source files (note: common.c is at $(ROOT)src/common.c, not in a subdir)
SRCS_ADDON  := src/$(ADDON).c
SRCS_COMMON := $(ROOT)src/common.c \
               $(ROOT)src/common/ctrlmsg.c \
               $(ROOT)src/common/ph_shm.c
SRCS        := $(SRCS_ADDON) $(SRCS_COMMON)

OBJS := $(SRCS:.c=.o)

.PHONY: all clean

all: $(SO)

$(SO): $(OBJS)
	$(CC) -shared -o $@ $(OBJS) $(LDFLAGS) -ldl

# Compile
%.o: %.c
	$(CC) $(CFLAGS) $(INCS) -c $< -o $@

clean:
	rm -f $(SO) $(OBJS)
