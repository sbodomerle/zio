LINUX ?= /lib/modules/$(shell uname -r)/build

EXTRA_CFLAGS += -I$(obj)/../include/ -DDEBUG

obj-m = zio-zero.o
obj-m += zio-gpio.o
obj-m += zio-ad7888.o

all:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) modules
