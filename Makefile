CFLAGS=-Wall -Wextra -Wswitch-enum -std=c99 -pedantic -D_BSD_SOURCE -D_POSIX_C_SOURCE=199506 -g -O2 `pkg-config --cflags fuse sqlcipher` -DSQLITE_HAS_CODEC -DFUSE_USE_VERSION=26
LDLIBS=`pkg-config --libs fuse sqlcipher`

# Some platforms may need this to run tests.
CFLAGS+=-pthread
LDLIBS+=-lpthread

# To run leak tests (only works on glibc)
#CFLAGS+=-DHAVE_MTRACE
#MALLOC_TRACE_FILE=malloc_trace.txt

# Some platforms may need:
#LDLIBS+=-lrt

COMMON_OBJS=logging.o sqlfs.o sqlfuse.o intmap.o
ALL_OBJS=$(COMMON_OBJS) main.o sqlfuse_tests.o
SQLFUSE_OBJS=$(COMMON_OBJS) main.o
INTMAP_TESTS_OBJS=$(COMMON_OBJS) test_common.o intmap_tests.o
SQLFUSE_TESTS_OBJS=$(COMMON_OBJS) test_common.o sqlfuse_tests.o
TESTS=intmap_tests sqlfuse_tests

all: sqlfuse

test: run_unit_tests

tests: $(TESTS)

run_unit_tests: tests
	./intmap_tests
	./sqlfuse_tests

run_leak_tests: tests
	./run_leak_tests.sh $(TESTS)

sqlfs.o: sqlfs.c sqlfs.h sqlfs_schema.h
	$(CC) $(CFLAGS) -o $@ -c $<

sqlfuse: $(SQLFUSE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(SQLFUSE_OBJS) $(LDLIBS)

intmap_tests: $(INTMAP_TESTS_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(INTMAP_TESTS_OBJS) $(LDLIBS)

sqlfuse_tests: $(SQLFUSE_TESTS_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(SQLFUSE_TESTS_OBJS) $(LDLIBS)

clean:
	rm -f $(ALL_OBJS)

distclean: clean
	rm -f sqlfuse intmap_tests sqlfuse_tests

.PHONY: all test run_unit_tests run_leak_tests clean distclean
