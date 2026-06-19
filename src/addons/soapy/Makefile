CC ?= cc
CFLAGS ?= -O2
LDFLAGS ?= -pthread
PH_CFLAGS := -std=c11 -Wall -Wextra -fPIC -pthread
INCS = -I../../../include

NAME := soapy
SO := ph-lib$(NAME).so

HAVE_SOAPY := $(shell pkg-config --exists SoapySDR >/dev/null 2>&1 && echo 1 || echo 0)
SOAPY_CFLAGS := $(shell pkg-config --cflags SoapySDR 2>/dev/null)
SOAPY_LIBS := $(shell pkg-config --libs SoapySDR 2>/dev/null || echo -lSoapySDR)

.PHONY: all clean

SRCS_SO := src/$(NAME).c \
../../../src/common.c \
../../../src/common/ctrlmsg.c \
../../../src/common/ph_shm.c \
../../../src/common/ph_subs.c

ifeq ($(HAVE_SOAPY),1)
all: $(SO)
$(SO): $(SRCS_SO)
	$(CC) $(PH_CFLAGS) $(CFLAGS) $(SOAPY_CFLAGS) $(INCS) -shared -o $@ $^ $(LDFLAGS) $(SOAPY_LIBS) -ldl
else
all:
	@echo "[soapy] skipping: missing SoapySDR development package (pkg-config SoapySDR). Set REQUIRE_DEPS=1 to fail."
	@test "$(REQUIRE_DEPS)" != "1"
endif

clean:
	rm -f $(SO)
