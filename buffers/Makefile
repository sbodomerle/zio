LINUX ?= /lib/modules/$(shell uname -r)/build

EXTRA_CFLAGS += -I$(obj)/../include/

# zio-buf-kmalloc.o is now part of zio-core

all:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) modules
