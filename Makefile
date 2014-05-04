LINUX ?= /lib/modules/$(shell uname -r)/build

zio-y := core.o chardev.o sysfs.o misc.o
zio-y += bus.o objects.o helpers.o dma.o config.o
zio-y += buffers/zio-buf-kmalloc.o triggers/zio-trig-user.o

# Waiting for Kconfig...
CONFIG_ZIO_SNIFF_DEV:=y

zio-$(CONFIG_ZIO_SNIFF_DEV) += sniff-dev.o

obj-m = zio.o
obj-m += drivers/
obj-m += buffers/
obj-m += triggers/

GIT_VERSION = $(shell cd $(src); git describe --dirty --long --tags)

# For this CSM_VERSION, please see ohwr.org/csm documentation
ifdef CONFIG_CSM_VERSION
  ccflags-y += -D"CERN_SUPER_MODULE=MODULE_VERSION(\"$(CONFIG_CSM_VERSION)\")"
else
  ccflags-y += -DCERN_SUPER_MODULE=""
endif

# WARNING: the line below doesn't work in-kernel if you compile with O=
ccflags-y += -I$(src)/include/ -DGIT_VERSION=\"$(GIT_VERSION)\"

ccflags-$(CONFIG_ZIO_DEBUG) += -DDEBUG

all: modules tools

modules:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd)

modules_install:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) $@


coccicheck:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) coccicheck


.PHONY: tools

tools:
	$(MAKE) -C tools M=$(shell /bin/pwd)

# this make clean is ugly, I'm aware...
clean:
	rm -rf `find . -name \*.o -o -name \*.ko -o -name \*~ `
	rm -rf `find . -name Module.\* -o -name \*.mod.c`
	rm -rf .tmp_versions modules.order
	$(MAKE) -C tools clean
