CFLAGS=-Wall -Wextra -Wswitch-enum -std=c99 -pedantic -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_POSIX_C_SOURCE=199506 -g -O2 `pkg-config --cflags fuse sqlcipher` -DSQLITE_HAS_CODEC -DFUSE_USE_VERSION=26
LDLIBS=`pkg-config --libs fuse sqlcipher`

# Some platforms may need this to run tests.
CFLAGS+=-pthread
LDLIBS+=-lpthread

# Some platforms may need:
#LDLIBS+=-lrt

# Enable testing with mtrace (only works on glibc)
#CFLAGS+=-DHAVE_MTRACE

# Enable compiling the main binary with mtrace. (Never enable this in production
# builds! It should only be used for memory debugging.)
#CFLAGS+=-DWITH_MTRACE

COMMON_OBJS=logging.o sqlfs.o sqlfuse.o intmap.o
SQLFUSE_OBJS=$(COMMON_OBJS) main.o

INTMAP_TESTS_OBJS=$(COMMON_OBJS) test_common.o intmap_tests.o
SQLFS_TESTS_OBJS=$(COMMON_OBJS) test_common.o sqlfs_tests.o
SQLFUSE_INTERNAL_TESTS_OBJS=$(COMMON_OBJS) test_common.o sqlfuse_internal_tests.o
SQLFUSE_EXTERNAL_TESTS_OBJS=$(COMMON_OBJS) test_common.o sqlfuse_external_tests.o

LEAK_TESTABLE_TESTS=intmap_tests sqlfs_tests
UNIT_TESTS=$(LEAK_TESTABLE_TESTS) sqlfuse_internal_tests sqlfuse_external_tests

bin: sqlfuse

all: bin tests

test: run_unit_tests run_cli_tests

tests: $(UNIT_TESTS)

run_unit_tests: $(UNIT_TESTS)
	./run_unit_tests.sh $(UNIT_TESTS)

run_cli_tests: cli_tests.sh sqlfuse
	./cli_tests.sh

# For these tests to pass, uncomment the line with -DHAVE_MTRACE above.
run_leak_tests: $(LEAK_TESTABLE_TESTS)
	./run_leak_tests.sh $(LEAK_TESTABLE_TESTS)

run_static_analysis: PVS-Studio-tasks.txt
	cat PVS-Studio-tasks.txt

PVS-Studio-tasks.txt: *.[hc]
	# Runs PVS Studio static source code analysis.
	# This requires all the tools invoked below to be installed.
	# Source files will be modified and some temporary files will be
	# generated, so be careful when checking in changes!
	how-to-use-pvs-studio-free -c 2 *.c
	pvs-studio-analyzer trace -- make clean all
	pvs-studio-analyzer analyze -o PVS-Studio.log
	plog-converter -t tasklist PVS-Studio.log >PVS-Studio-tasks.txt

sqlfs.o: sqlfs.c sqlfs.h sqlfs_schema.h
	$(CC) $(CFLAGS) -o $@ -c $<

sqlfuse: $(SQLFUSE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(SQLFUSE_OBJS) $(LDLIBS)

intmap_tests: $(INTMAP_TESTS_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(INTMAP_TESTS_OBJS) $(LDLIBS)

sqlfs_tests: $(SQLFS_TESTS_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(SQLFS_TESTS_OBJS) $(LDLIBS)

sqlfuse_internal_tests: $(SQLFUSE_INTERNAL_TESTS_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(SQLFUSE_INTERNAL_TESTS_OBJS) $(LDLIBS)

sqlfuse_external_tests: $(SQLFUSE_EXTERNAL_TESTS_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(SQLFUSE_EXTERNAL_TESTS_OBJS) $(LDLIBS)

clean:
	rm -f ./*.o

distclean: clean
	rm -f sqlfuse $(UNIT_TESTS)

.PHONY: bin all test tests run_unit_tests run_cli_tests run_leak_tests run_static_analysis clean distclean
