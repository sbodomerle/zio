LINUX ?= /lib/modules/$(shell uname -r)/build

EXTRA_CFLAGS += -I$(obj)/../include/

obj-m = zio-buf-kmalloc.o

all:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) modules
