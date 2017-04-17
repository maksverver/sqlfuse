CFLAGS=-Wall -Wextra -std=c99 -g -O2 `pkg-config --cflags fuse sqlcipher` -DSQLITE_HAS_CODEC -D_BSD_SOURCE -DFUSE_USE_VERSION=26
LDLIBS=`pkg-config --libs fuse sqlcipher`
OBJS=logging.o main.o sqlfs.o sqlfuse.o

all: main

main: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

clean:
	rm -f $(OBJS)

distclean: clean
	rm -f main

.PHONY: all clean distclean
