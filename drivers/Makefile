LINUX ?= /lib/modules/$(shell uname -r)/build

GIT_VERSION = $(shell cd $(src); git describe --dirty --long --tags)

# For this CSM_VERSION, please see ohwr.org/csm documentation
ifdef CONFIG_CSM_VERSION
  ccflags-y += -D"CERN_SUPER_MODULE=MODULE_VERSION(\"$(CONFIG_CSM_VERSION)\")"
else
  ccflags-y += -DCERN_SUPER_MODULE=""
endif

ccflags-y += -I$(src)/../include/ -DGIT_VERSION=\"$(GIT_VERSION)\"
ccflags-y += $(ZIO_VERSION)
ccflags-$(CONFIG_ZIO_DEBUG) += -DDEBUG

obj-m = zio-zero.o
obj-m += zio-loop.o
obj-m += zio-irq-tdc.o
obj-m += zio-fake-dtc.o
obj-m += zio-mini.o
obj-m += zio-gpio.o

ifdef CONFIG_USB
obj-m += zio-vmk8055.o
endif

ifdef CONFIG_SPI
obj-m += zio-ad788x.o
endif
