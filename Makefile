CFLAGS=-Wall -Wextra -std=c99 -g -O2 `pkg-config --cflags fuse sqlcipher` -DSQLITE_HAS_CODEC -D_BSD_SOURCE -DFUSE_USE_VERSION=26
LDLIBS=`pkg-config --libs fuse sqlcipher`

# Some platforms may need this to run tests.
CFLAGS+=-pthread
LDLIBS+=-lpthread

COMMON_OBJS=logging.o sqlfs.o sqlfuse.o
ALL_OBJS=$(COMMON_OBJS) main.o tests.o
SQLFUSE_OBJS=$(COMMON_OBJS) main.o
TESTS_OBJS=$(COMMON_OBJS) tests.o

all: sqlfuse

test: tests
	./tests

sqlfuse: $(SQLFUSE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(SQLFUSE_OBJS) $(LDLIBS)

tests: $(TESTS_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(TESTS_OBJS) $(LDLIBS)

clean:
	rm -f $(ALL_OBJS)

distclean: clean
	rm -f sqlfuse tests

.PHONY: all test clean distclean
