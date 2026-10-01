# picks the allocator. exactly one src/alloc_<name>.c gets linked, interface in include/alloc_strategy.h.
#
#   make                                      # linear scan, the original
#   make BSFS_ALLOC=heap                      # min-heap of recycled blocks
#   make BSFS_ALLOC=heap BSFS_ALLOC_STATS=y   # plus the unmount counters
#
# either flag changes -D on every object, so kbuild rebuilds by itself. no `make clean` needed.

ifneq ($(KERNELRELEASE),)
# kbuild pass, BSFS_ALLOC and BSFS_ALLOC_STATS are exported by the pass below

BSFS_ALLOC ?= linear

bsfs2-src-objs := \
	alloc.o \
	alloc_$(BSFS_ALLOC).o \
	bitmap.o \
	btree.o \
	dir.o \
	dirent.o \
	file.o \
	inode.o \
	reclaim_stats.o \
	rec_entry.o

obj-m += bsfs2.o

bsfs2-y := bsfs.o $(addprefix src/,$(bsfs2-src-objs))

ccflags-y += -include $(src)/include/pr_fmt.h
ccflags-y += -DBSFS_ALLOC_STRATEGY_NAME=\"$(BSFS_ALLOC)\"
ccflags-y += $(if $(BSFS_ALLOC_STATS),-DBSFS_ALLOC_STATS)

else
# KDIR and optionally BSFS_ALLOC come from local.mk (also sourced by start_qemu.sh and tests/smoke.sh),
# else the running kernel's build tree. local.mk must parse as sh and make, so plain VAR=value,
# no quotes, ${HOME} not $HOME.
-include local.mk
KDIR ?= /lib/modules/$(shell uname -r)/build
export KDIR

BSFS_ALLOC ?= linear
export BSFS_ALLOC BSFS_ALLOC_STATS

ifeq ($(wildcard src/alloc_$(BSFS_ALLOC).c),)
  $(error unknown BSFS_ALLOC='$(BSFS_ALLOC)', no src/alloc_$(BSFS_ALLOC).c. \
    Available: $(patsubst src/alloc_%.c,%,$(wildcard src/alloc_*.c)))
endif

MKFS = mkfs.bsfs
RECOVERY = recovery.bsfs

all: $(MKFS) $(RECOVERY)
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

TOOLS_CFLAGS = -Wall -Wextra -O2
TOOLS_HDRS = tools/libbsfs.h include/bsfs_shared.h

tools/%.o: tools/%.c $(TOOLS_HDRS)
	$(CC) $(TOOLS_CFLAGS) -c -o $@ $<

$(MKFS): tools/mkfs.o tools/libbsfs.o
	$(CC) -o $@ $^

$(RECOVERY): tools/recovery.o tools/libbsfs.o
	$(CC) -o $@ $^

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	$(MAKE) -C bench clean
	rm -f $(MKFS) $(RECOVERY) tools/*.o

bench:
	$(MAKE) -C bench

bench-check:
	$(MAKE) -C bench check

compile_commands.json:
	$(MAKE) -C $(KDIR) M=$(CURDIR) compile_commands.json

.PHONY: all clean compile_commands.json bench bench-check
endif
