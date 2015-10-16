
zio-y := core.o chardev.o sysfs.o misc.o
zio-y += bus.o objects.o helpers.o dma.o
zio-y += buffers/zio-buf-kmalloc.o triggers/zio-trig-user.o

# Waiting for Kconfig...
CONFIG_ZIO_SNIFF_DEV:=y

zio-$(CONFIG_ZIO_SNIFF_DEV) += sniff-dev.o

obj-m = zio.o
obj-m += drivers/
obj-m += buffers/
obj-m += triggers/

# src is defined byt the kernel Makefile, but we want to use it also in our
# local Makefile (tools, lib)

# For this CSM_VERSION, please see ohwr.org/csm documentation
ifdef CONFIG_CSM_VERSION
  ccflags-y += -D"CERN_SUPER_MODULE=MODULE_VERSION(\"$(CONFIG_CSM_VERSION)\")"
else
  ccflags-y += -DCERN_SUPER_MODULE=""
endif

# WARNING: the line below doesn't work in-kernel if you compile with O=
ccflags-y += -I$(src)/include/ -DGIT_VERSION=\"$(GIT_VERSION)\"
ccflags-y += $(ZIO_VERSION)
ccflags-$(CONFIG_ZIO_DEBUG) += -DDEBUG
