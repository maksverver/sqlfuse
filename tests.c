// Unit tests for fusesql.
//
// Note: these tests create temporary directories named /tmp/test-xxx-yy, and
// mounts the filesystem for testing at /tmp/test-xxx-yyy/mnt. If a test fails,
// it may leave the test directory behind. You'll have to clean it up manually.
// To unmount, use: fusermount -u /tmp/test-xxx-yyy/mnt.
//
// To use a different directory, set the TMPDIR environmental variable to the
// directory root to use instead of /tmp.

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "fuse.h"
#include "fuse_lowlevel.h"

#include "logging.h"
#include "sqlfs.h"
#include "sqlfuse.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int failures = 0;
static char *testdir;
static char *mountpoint;
static char *database;
static struct sqlfs *sqlfs;
static struct fuse_args fuse_args;
static struct fuse_chan *fuse_chan;
static struct fuse_session *fuse_session;
static pthread_t fuse_thread;

#define EXPECT_EQ(x, y) expect_eq(__FILE__, __LINE__, __func__, #x, #y, (x), (y))
#define EXPECT(x) expect_eq(__FILE__, __LINE__, __func__, #x, "true", (bool)(x), true);

void expect_eq(const char *file, int line, const char *func, const char *expr1, const char *expr2, int value1, int value2) {
  if (value1 == value2) return;
  fprintf(stderr, "[%s:%d] Assertion failed in %s(). Expected %s (%d) to be equal to %s (%d).\n",
      file, line, func, expr1, value1, expr2, value2);
  ++failures;
}

#ifdef __GNUC__
static char *aprintf(const char *format, ...)
  __attribute__((format(printf, 1, 2)));
#endif

// Formats a string into a newly allocated buffer. The result of aprintf()
// should be free()d by the caller.
static char *aprintf(const char *format, ...) {
  va_list ap;

  va_start(ap, format);
  int n = vsnprintf(NULL, 0, format, ap);
  va_end(ap);

  CHECK(n >= 0);
  char *buf = malloc(n + 1);
  CHECK(buf);

  va_start(ap, format);
  vsnprintf(buf, n + 1, format, ap);
  va_end(ap);

  return buf;
}

static bool timespec_eq(const struct timespec *a, const struct timespec *b) {
  return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

static bool timespec_le(const struct timespec *a, const struct timespec *b) {
  return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec <= b->tv_nsec);
}

static void global_setup() {
  const char *tempdir = getenv("TMPDIR");
  if (tempdir == NULL || *tempdir == '\0') {
    tempdir = "/tmp";
  }
  testdir = aprintf("%s/test-%d-%6d", tempdir, (int)getpid(), (int)(time(NULL)%1000000));
  mountpoint = aprintf("%s/mnt", testdir);
  database = aprintf("%s/db", testdir);
  CHECK(mkdir(testdir, 0700) == 0);
  CHECK(mkdir(mountpoint, 0700) == 0);
  CHECK(strlen(mountpoint) + 1000 < PATH_MAX);
}

static void global_teardown() {
  CHECK(rmdir(mountpoint) == 0);
  free(mountpoint);
  mountpoint = NULL;
  CHECK(rmdir(testdir) == 0);
  free(testdir);
  testdir = NULL;
  free(database);
  database = NULL;
}

static void *fuse_thread_func(void *arg) {
  return (void*)(ssize_t)fuse_session_loop((struct fuse_session*)arg);
}

static void setup() {
  sqlfs = sqlfs_create(database, NULL /*password*/, 022 /* umask */, geteuid(), getegid());
  CHECK(sqlfs);

  char *argv[2] = {"test", NULL};
  fuse_args.argc = 1;
  fuse_args.argv = argv;
  fuse_args.allocated = 0;
  fuse_chan = fuse_mount(mountpoint, &fuse_args);
  CHECK(fuse_chan);
  fuse_session = fuse_lowlevel_new(&fuse_args, &sqlfuse_ops, sizeof(sqlfuse_ops), sqlfs /* userdata */);
  CHECK(fuse_session);
  fuse_session_add_chan(fuse_session, fuse_chan);
  fuse_set_signal_handlers(fuse_session);

  CHECK(pthread_create(&fuse_thread, NULL /* attr */, fuse_thread_func, fuse_session /* arg */) == 0);
}

static void teardown() {
  pthread_kill(fuse_thread, SIGHUP);

  void *retval = NULL;
  CHECK(pthread_join(fuse_thread, &retval) == 0);
  EXPECT_EQ((int)(ssize_t)retval, 0);

  fuse_remove_signal_handlers(fuse_session);
  fuse_session_remove_chan(fuse_chan);
  fuse_session_destroy(fuse_session);
  fuse_unmount(mountpoint, fuse_chan);
  fuse_opt_free_args(&fuse_args);
  fuse_chan = NULL;
  fuse_session = NULL;
  memset(&fuse_args, 0, sizeof(fuse_args));

  sqlfs_destroy(sqlfs);
  sqlfs = NULL;
  CHECK(unlink(database) == 0);
}

static void test_basic() {
  setup();
  // Doesn't do anything. Just verifies the test framework works.
  teardown();
}

// This test may fail if the realtime clock rewinds during the test!
static void test_rootdir() {
  struct timespec time_before_setup;
  CHECK(clock_gettime(CLOCK_REALTIME, &time_before_setup) == 0);

  setup();

  struct timespec time_after_setup;
  CHECK(clock_gettime(CLOCK_REALTIME, &time_after_setup) == 0);

  struct stat st = {0};
  stat(mountpoint, &st);
  EXPECT_EQ(stat(mountpoint, &st), 0);
  EXPECT_EQ(st.st_ino, 1);
  EXPECT_EQ(st.st_mode, 0755 | S_IFDIR);
  EXPECT_EQ(st.st_nlink, 2);
  EXPECT_EQ(st.st_uid, geteuid());
  EXPECT_EQ(st.st_gid, getegid());
  EXPECT_EQ(st.st_size, 0);
  EXPECT_EQ(st.st_blksize, 4096);
  EXPECT_EQ(st.st_blocks, 0);
  EXPECT(timespec_eq(&st.st_atim, &st.st_mtim));
  EXPECT(timespec_eq(&st.st_ctim, &st.st_mtim));
  EXPECT(timespec_le(&time_before_setup, &st.st_mtim));
  EXPECT(timespec_le(&st.st_mtim, &time_after_setup));
  teardown();
}

static void test_mkdir() {
  char buf[PATH_MAX];

  setup();

  snprintf(buf, sizeof(buf), "%s/subdir", mountpoint);
  EXPECT_EQ(mkdir(buf, 0770), 0);
  struct stat st = {0};
  EXPECT_EQ(stat(buf, &st), 0);
  EXPECT_EQ(st.st_mode, 0750 | S_IFDIR);  // umask has been applied

  // TODO: Add tests with invalid names; if possible, match all
  // TODO: Maybe add test for mkdirat? What happens if parent dir is already deleted?

  teardown();
}

static const struct Test {
  const char *name;
  void (*func)(void);
} tests[] = {
#define TEST(x) {#x, &test_##x}
  TEST(basic),
  TEST(rootdir),
  TEST(mkdir),
#undef TEST
  {NULL, NULL}};

static bool run_tests() {
  int failed_tests = 0;
  failures = 0;
  for (const struct Test *test = tests; test->func != NULL; ++test) {
    int failures_before = failures;
    test->func();
    bool failed = failures != failures_before;
    failed_tests += failed;
    fprintf(stderr, "Test %s %s.\n", test->name, failed ? "failed" : "passed");
  }
  fprintf(stderr, "%d test failures. %d tests failed.\n", failures, failed_tests);
  return failed_tests == 0;
}

int main(int argc, char *argv[]) {
  (void)argc, (void)argv;  // Unused.
  global_setup();
  bool all_pass = run_tests();
  global_teardown();
  return all_pass ? 0 : 1;
}
