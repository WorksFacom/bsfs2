obj-m += bsfs.o

# bsfs-objs := ...

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

compile_commands.json:
	make -C $(KDIR) M=$(PWD) compile_commands.json

.PHONY: all clean compile_commands.json
