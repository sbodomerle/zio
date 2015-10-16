# For this CSM_VERSION, please see ohwr.org/csm documentation
ifdef CONFIG_CSM_VERSION
  ccflags-y += -D"CERN_SUPER_MODULE=MODULE_VERSION(\"$(CONFIG_CSM_VERSION)\")"
else
  ccflags-y += -DCERN_SUPER_MODULE=""
endif

ccflags-y += -I$(src)/../include/ -DGIT_VERSION=\"$(GIT_VERSION)\"
ccflags-$(CONFIG_ZIO_DEBUG) += -DDEBUG

# zio-buf-kmalloc.o is now part of zio-core
obj-m = zio-buf-vmalloc.o
