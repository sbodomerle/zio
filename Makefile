LINUX ?= /lib/modules/$(shell uname -r)/build

zio-core-objs := zio-dev.o zio-sys.o zio-buf.o
obj-m = zio-core.o
obj-m += drivers/
obj-m += buffers/
obj-m += triggers/

EXTRA_CFLAGS += -I$(obj)/include/

hostprogs-y := zio-dump

HOST_EXTRACFLAGS += -I$(M)/include/

all: modules user

modules:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd)

# This is ugly, please forgive me by now
user: $(hostprogs-y)

zio-dump: zio-dump.c
	$(CC) -Wall -Iinclude $^ -o $@

# this make clean is ugly, I'm aware...
clean:
	rm -rf `find . -name \*.o -o -name \*.ko -o -name \*~ `
	rm -rf `find . -name Module.\* -o -name \*.mod.c`
	rm -rf .tmp_versions modules.order
