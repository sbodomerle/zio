LINUX ?= /lib/modules/$(shell uname -r)/build

GIT_VERSION = $(shell cd $(src); git describe --dirty --long --tags)

ccflags-y += -I$(obj)/../include/ -DGIT_VERSION=\"$(GIT_VERSION)\"
ccflags-$(CONFIG_ZIO_DEBUG) += -DDEBUG

# zio-buf-kmalloc.o is now part of zio-core
obj-m = zio-buf-vmalloc.o
obj-m += zio-buf-sock.o

zio-buf-sock-objs = sock-syscall.o sock-buffer.o sock-nic.o
