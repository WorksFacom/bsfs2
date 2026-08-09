bsfs2-src-objs := \
	alloc.o \
	alloc_linear.o \
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
ccflags-y += $(if $(BSFS_ALLOC_STATS),-DBSFS_ALLOC_STATS)

# KDIR comes from .env (the same file start_qemu.sh / tests/smoke.sh source),
# falling back to the running kernel's build tree.
-include .env
export KDIR BSFS_ALLOC_STATS
KDIR ?= /lib/modules/$(shell uname -r)/build

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
