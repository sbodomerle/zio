
# build user-space tools for zio

HOST_EXTRACFLAGS += -I$(M)/include/

hostprogs-y := zio-dump

# The following is ugly, please forgive me by now
user: $(hostprogs-y)

clean:
	rm -f $(hostprogs-y) *~ *.o

%: %.c
	$(CC) $(HOST_EXTRACFLAGS) -Wall $^ -o $@

