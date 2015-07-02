
# build user-space tools for zio

CFLAGS = -I$(M)/include/ -Wall $(ZIO_VERSION) $(EXTRACFLAGS)
CC ?= $(CROSS_COMPILE)gcc

progs := zio-dump
progs += zio-cat-file
progs += test-dtc

# The following is ugly, please forgive me by now
user: $(progs)

clean:
	rm -f $(progs) *~ *.o

%: %.c
	$(CC) $(CFLAGS) $^ -o $@
