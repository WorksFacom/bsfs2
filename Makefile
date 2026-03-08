obj-m += bsfs.o
bsfs-objs := src/bsfs.o

KDIR ?= /lib/modules/$(shell uname -r)/build

SRC_FILES = $(shell find src -type f -name '*.c')

MKFS = mkfs.bsfs

all: $(MKFS)
	make -C $(KDIR) M=$(PWD) modules

$(MKFS): tools/mkfs.c
	$(CC) -Wall -o $@ $< -Isrc/include $(SRC_FILES)

clean:
	make -C $(KDIR) M=$(PWD) clean

compile_commands.json:
	make -C $(KDIR) M=$(PWD) compile_commands.json

.PHONY: all clean compile_commands.json
