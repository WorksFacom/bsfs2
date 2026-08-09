# KDIR comes from .env (the same file start_qemu.sh / tests/smoke.sh source),
# falling back to the running kernel's build tree. BSFS_ALLOC can be pinned
# there too, so the QEMU flow does not have to repeat it on every build.
-include .env
export KDIR
KDIR ?= /lib/modules/$(shell uname -r)/build

# Which block-allocation strategy to compile in. Exactly one src/alloc_<name>.c
# is linked; see include/alloc_strategy.h for the interface they implement.
#
#   make                            # linear scan, the historical behaviour
#   make BSFS_ALLOC=heap            # min-heap of recycled blocks
#   make BSFS_ALLOC=heap BSFS_ALLOC_STATS=y   # ... plus the unmount counters
#
# Changing either flag changes -D on every object, so kbuild rebuilds the whole
# module on its own - no `make clean` needed to switch strategies.
BSFS_ALLOC ?= linear
export BSFS_ALLOC BSFS_ALLOC_STATS

ifeq ($(wildcard $(src)/src/alloc_$(BSFS_ALLOC).c $(PWD)/src/alloc_$(BSFS_ALLOC).c),)
  $(error unknown BSFS_ALLOC='$(BSFS_ALLOC)' - no src/alloc_$(BSFS_ALLOC).c. \
    Available: $(patsubst src/alloc_%.c,%,$(wildcard src/alloc_*.c)))
endif

bsfs2-src-objs := \
	alloc.o \
	alloc_$(BSFS_ALLOC).o \
	bitmap.o \
	bspan_queue.o \
	btree.o \
	dir.o \
	dirent.o \
	file.o \
	inode.o \
	reclaim_stats.o \
	rec_entry.o
	# rec_gc.o

obj-m += bsfs2.o

bsfs2-objs := bsfs.o $(addprefix src/,$(bsfs2-src-objs))

ccflags-y += -include $(PWD)/include/pr_fmt.h
ccflags-y += -DBSFS_ALLOC_STRATEGY_NAME=\"$(BSFS_ALLOC)\"
ccflags-y += $(if $(BSFS_ALLOC_STATS),-DBSFS_ALLOC_STATS)

SRC_FILES = $(shell find src -type f -name '*.c')

MKFS = mkfs.bsfs

all: $(MKFS)
	$(MAKE) -C $(KDIR) M=$(PWD) modules

$(MKFS): tools/mkfs.c
	$(CC) -Wall -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(MAKE) -C bench clean
	rm -f $(MKFS)

bench:
	$(MAKE) -C bench

bench-check:
	$(MAKE) -C bench check

compile_commands.json:
	make -C $(KDIR) M=$(PWD) compile_commands.json

.PHONY: all clean compile_commands.json bench bench-check
