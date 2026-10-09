# Diagnostic harnesses. See README.md.
#
# Host build:
#     make
#
# Board build -- point CC at a cross compiler and SYSROOT at a sysroot carrying
# the board's gbm/libdrm/Vulkan libraries, e.g. the one emb stages:
#
#     out=$(emb cross . --target rpi5-trixie)
#     make CC=<the cc it prints> SYSROOT=<the target sysroot it prints> \
#          CFLAGS="-O2 -Wall -Wextra -mcpu=cortex-a76"
#
# Override MULTIARCH for a target that is not aarch64.
#
# Nothing in the product links these. They are built by hand and run by hand,
# and are kept buildable rather than merely archived: a harness that no longer
# compiles is not evidence of anything. CI builds `compile-only` for that
# reason.

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
SYSROOT ?=
MULTIARCH ?= aarch64-linux-gnu

# The repo vendors these, so compiling needs no system Vulkan dev package --
# which is what lets the CI gate run on a runner with no GPU stack.
VULKAN_INCLUDE ?= $(CURDIR)/../../third_party/Vulkan-Headers/include
override CFLAGS += -I$(VULKAN_INCLUDE)

# --sysroot on its own is not enough for a Debian-style sysroot: the libc and
# libdrm headers live under subdirectories the compiler does not search by
# default, and the build dies on bits/libc-header-start.h or drm_fourcc.h,
# which reads as a broken sysroot rather than a missing -I. rpath-link lets the
# linker resolve the transitive DT_NEEDED of the gbm/Vulkan stubs without
# baking a host path into the binary.
#
# `override` is load-bearing: a command-line `make CFLAGS=...` beats every
# plain assignment in a makefile, `+=` included, so without it the natural way
# to cross-build silently drops these.
ifneq ($(SYSROOT),)
  override CFLAGS += --sysroot=$(SYSROOT) \
             -I$(SYSROOT)/usr/include/$(MULTIARCH) \
             -I$(SYSROOT)/usr/include/libdrm
  override LDFLAGS += --sysroot=$(SYSROOT) -B$(SYSROOT)/usr/lib/$(MULTIARCH) \
             -L$(SYSROOT)/usr/lib/$(MULTIARCH) -L$(SYSROOT)/lib/$(MULTIARCH) \
             -Wl,-rpath-link,$(SYSROOT)/usr/lib/$(MULTIARCH):$(SYSROOT)/lib/$(MULTIARCH)
else
  override CFLAGS += $(shell pkg-config --cflags gbm libdrm)
endif

all: dmabuf_import_probe

dmabuf_import_probe: dmabuf_import_probe.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) -lgbm -ldrm -lvulkan

# The CI gate. Compiles without linking, so it needs gbm/libdrm headers but no
# Vulkan loader, no driver and no GPU on the runner.
compile-only: dmabuf_import_probe.c
	$(CC) $(CFLAGS) -Werror -c $< -o /dev/null

clean:
	rm -f dmabuf_import_probe

.PHONY: all compile-only clean
