bsfs2-src-objs := \
	alloc.o \
	bitmap.o \
	bspan_queue.o \
	btree.o \
	dir.o \
	dirent.o \
	file.o \
	inode.o \
	rec_entry.o
	# rec_gc.o

obj-m += bsfs2.o

bsfs2-objs := bsfs.o $(addprefix src/,$(bsfs2-src-objs))

ccflags-y += -include $(PWD)/include/pr_fmt.h

# KDIR comes from .env (the same file start_qemu.sh / tests/smoke.sh source),
# falling back to the running kernel's build tree.
-include .env
export KDIR
KDIR ?= /lib/modules/$(shell uname -r)/build

SRC_FILES = $(shell find src -type f -name '*.c')

MKFS = mkfs.bsfs

all: $(MKFS)
	$(MAKE) -C $(KDIR) M=$(PWD) modules

$(MKFS): tools/mkfs.c
	$(CC) -Wall -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(MKFS)

compile_commands.json:
	make -C $(KDIR) M=$(PWD) compile_commands.json

.PHONY: all clean compile_commands.json
