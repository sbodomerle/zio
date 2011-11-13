LINUX ?= /lib/modules/$(shell uname -r)/build

EXTRA_CFLAGS += -I$(obj)/../include/

obj-m = zio-trig-timer.o
obj-m += zio-trig-irq.o
obj-m += zio-trig-user.o

all:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) modules
