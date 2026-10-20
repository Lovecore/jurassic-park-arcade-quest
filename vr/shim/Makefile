# vr_shim.so — Phase 1 VR enablement shim for JP2015 (32-bit Linux game)
# Build on i386 Linux directly, or on x86_64 with gcc-multilib installed.

CC      ?= gcc
# -m32 only on 64-bit hosts; on a native i386 toolchain it is invalid/redundant.
ifeq ($(shell uname -m),i686)
M32     :=
else
M32     := -m32
endif
CFLAGS  := $(M32) -O2 -fPIC -Wall -Wextra -Wno-unused-parameter
# Both hash styles: box64 (box32) looks symbols up via DT_HASH.
LDFLAGS := -shared -Wl,--hash-style=both -ldl -lpthread
SRCS    := shim.c gl_compat.c io_emu.c paths.c

all: vr_shim.so

vr_shim.so: $(SRCS) shim.h got_fixups.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f vr_shim.so

.PHONY: all clean
