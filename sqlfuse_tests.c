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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fuse.h"
#include "fuse_lowlevel.h"

#include "logging.h"
#include "sqlfs.h"
#include "sqlfuse.h"
#include "test_common.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static bool enable_fuse_debug_logging;
static char *sqlite_path_for_dump;
static char *testdir;
static char *mountpoint;
static char *database;
static struct sqlfs *sqlfs;
static struct fuse_args fuse_args;
static struct fuse_chan *fuse_chan;
static struct fuse_session *fuse_session;
static pthread_t fuse_thread;

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
  testdir = aprintf("%s/test-%d-%06d", tempdir, (int)getpid(), (int)(time(NULL)%1000000));
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
  memset(&fuse_args, 0, sizeof(fuse_args));
  fuse_args.argc = 1;
  fuse_args.argv = argv;
  fuse_opt_parse(&fuse_args, NULL, NULL, NULL);
  if (enable_fuse_debug_logging) {
    fuse_opt_add_arg(&fuse_args, "-d");
  }
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

  if (sqlite_path_for_dump) {
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork() failed");
    } else if (pid == 0) {
      close(0);  // close stdin
      dup2(2, 1);  // redirect stdout to stderr
      execlp(sqlite_path_for_dump, sqlite_path_for_dump, database, ".dump", (char*) NULL);
      perror("exec() failed");
      exit(1);
    } else {  // pid > 0
      int status = 0;
      waitpid(pid, &status, 0);
      EXPECT_EQ(status, 0);
    }
  }

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

static nlink_t nlinks(const char *path) {
  struct stat st = {0};
  if (stat(path, &st) != 0) {
    fprintf(stderr, "stat([%s]) failed.\n", path);
    test_fail();
    return -1;
  }
  return st.st_nlink;
}

static void test_mkdir() {
  char buf[PATH_MAX];

  setup();

  EXPECT_EQ(nlinks(mountpoint), 2);

  snprintf(buf, sizeof(buf), "%s/subdir", mountpoint);
  EXPECT_EQ(mkdir(buf, 0770), 0);
  struct stat st = {0};
  EXPECT_EQ(stat(buf, &st), 0);
  EXPECT_EQ(st.st_mode, 0750 | S_IFDIR);  // umask has been applied
  EXPECT_EQ(st.st_nlink, 2);

  // Root directory link count has increased.
  EXPECT_EQ(nlinks(mountpoint), 3);

  // Cannot recreate directory that already exists.
  EXPECT_EQ(mkdir(buf, 0755), -1);
  EXPECT_EQ(errno, EEXIST);

  // Cannot create child directory in non-existent parent directory
  snprintf(buf, sizeof(buf), "%s/nonexistent/subdir", mountpoint);
  EXPECT_EQ(mkdir(buf, 0770), -1);
  EXPECT_EQ(errno, ENOENT);

  // TODO: Maybe add test for mkdirat? What happens if parent dir is already deleted?

  teardown();
}

static void test_rmdir() {
  struct stat st;
  char dir_foo[PATH_MAX];
  char dir_foo_bar[PATH_MAX];
  char dir_foo_baz[PATH_MAX];

  snprintf(dir_foo, sizeof(dir_foo), "%s/foo", mountpoint);
  snprintf(dir_foo_bar, sizeof(dir_foo_bar), "%s/foo/bar", mountpoint);
  snprintf(dir_foo_baz, sizeof(dir_foo_baz), "%s/foo/baz", mountpoint);

  setup();

  EXPECT_EQ(rmdir(dir_foo), -1);
  EXPECT_EQ(errno, ENOENT);

  EXPECT_EQ(rmdir(dir_foo_bar), -1);
  EXPECT_EQ(errno, ENOENT);

  EXPECT_EQ(mkdir(dir_foo, 0700), 0);
  EXPECT_EQ(mkdir(dir_foo_bar, 0700), 0);
  EXPECT_EQ(mkdir(dir_foo_baz, 0700), 0);

  EXPECT_EQ(nlinks(mountpoint),  3); // ".", "..", "foo"
  EXPECT_EQ(nlinks(dir_foo),     4); // "../foo", ".", "bar/..", "baz/.."
  EXPECT_EQ(nlinks(dir_foo_bar), 2); // ".", "../bar"
  EXPECT_EQ(nlinks(dir_foo_bar), 2); // ".", "../baz"

  EXPECT_EQ(rmdir(dir_foo), -1);
  EXPECT_EQ(errno, ENOTEMPTY);

  EXPECT_EQ(rmdir(dir_foo_bar), 0);
  EXPECT_EQ(stat(dir_foo_bar, &st), -1);
  EXPECT_EQ(errno, ENOENT);

  EXPECT_EQ(nlinks(dir_foo), 3);
  EXPECT_EQ(rmdir(dir_foo), -1);
  EXPECT_EQ(errno, ENOTEMPTY);

  EXPECT_EQ(rmdir(dir_foo_baz), 0);
  EXPECT_EQ(stat(dir_foo_baz, &st), -1);
  EXPECT_EQ(errno, ENOENT);

  // Cannot rmdir() a file. Use unlink() instead.
  EXPECT_EQ(mknod(dir_foo_bar, S_IFREG | 0600, 0), 0);
  EXPECT_EQ(rmdir(dir_foo_bar), -1);
  EXPECT_EQ(errno, ENOTDIR);

  // Files do not increase parent directory's hardlink count...
  EXPECT_EQ(nlinks(dir_foo), 2);

  // ... but they do prevent them from being deleted.
  EXPECT_EQ(rmdir(dir_foo), -1);
  EXPECT_EQ(errno, ENOTEMPTY);

  EXPECT_EQ(unlink(dir_foo_bar), 0);
  EXPECT_EQ(nlinks(dir_foo), 2);
  EXPECT_EQ(rmdir(dir_foo), 0);

  EXPECT_EQ(nlinks(mountpoint), 2);

  teardown();
}

static void test_mknod_unlink() {
  char buf[PATH_MAX];
  struct stat st;

  setup();

  snprintf(buf, sizeof(buf), "%s/foo", mountpoint);
  EXPECT_EQ(mknod(buf, S_IFREG | 0644, 0), 0);
  EXPECT_EQ(stat(buf, &st), 0);
  EXPECT_EQ(st.st_ino, 2);
  EXPECT_EQ(st.st_mode, S_IFREG | 0644);
  EXPECT_EQ(st.st_nlink, 1);
  EXPECT_EQ(st.st_size, 0);
  EXPECT(st.st_blksize > 0);
  EXPECT_EQ(st.st_blocks, 0);

  EXPECT_EQ(mknod(buf, S_IFREG | 0644, 0), -1);
  EXPECT_EQ(errno, EEXIST);

  // Invalid mode (symlinks not supportedy yet).
  EXPECT_EQ(mknod(buf, S_IFLNK | 0644, 0), -1);
  EXPECT_EQ(errno, EINVAL);

  snprintf(buf, sizeof(buf), "%s/foo/bar", mountpoint);
  EXPECT_EQ(mknod(buf, S_IFREG | 0644, 0), -1);
  EXPECT_EQ(errno, ENOTDIR);

  snprintf(buf, sizeof(buf), "%s/nonexistent/foo", mountpoint);
  EXPECT_EQ(mknod(buf, S_IFREG | 0644, 0), -1);
  EXPECT_EQ(errno, ENOENT);

  snprintf(buf, sizeof(buf), "%s/.", mountpoint);
  EXPECT_EQ(mknod(buf, S_IFREG | 0644, 0), -1);
  EXPECT_EQ(errno, EEXIST);

  snprintf(buf, sizeof(buf), "%s/bar", mountpoint);
  EXPECT_EQ(mknod(buf, S_IFREG | 0777, 0), 0);
  EXPECT_EQ(stat(buf, &st), 0);
  EXPECT_EQ(st.st_ino, 3);
  EXPECT_EQ(st.st_mode, S_IFREG | 0755);  // umask was applied
  EXPECT_EQ(st.st_nlink, 1);

  snprintf(buf, sizeof(buf), "%s/foo", mountpoint);
  EXPECT_EQ(unlink(buf), 0);

  EXPECT_EQ(unlink(buf), -1);
  EXPECT_EQ(errno, ENOENT);

  // Can't unlink a directory. Use rmdir() instead.
  EXPECT_EQ(mkdir(buf, 0775), 0);
  EXPECT_EQ(unlink(buf), -1);
  EXPECT_EQ(errno, EISDIR);
  EXPECT_EQ(rmdir(buf), 0);

  snprintf(buf, sizeof(buf), "%s/baz", mountpoint);
  EXPECT_EQ(mknod(buf, S_IFREG | 0600, 0), 0);

  // TODO: readdir to verify directory contains /bar, /baz
  // TODO: Maybe add test for mknodat? What happens if parent dir is already deleted?
  // TODO: Maybe add test for unlinkat? What happens if parent dir is already deleted?

  teardown();
}

static const struct test_case tests[] = {
#define TEST(x) {#x, &test_##x}
  TEST(basic),
  TEST(rootdir),
  TEST(mkdir),
  TEST(rmdir),
  TEST(mknod_unlink),
#undef TEST
  {NULL, NULL}};

int main(int argc, char *argv[]) {
  // Parse command line options.
  for (int opt; (opt = getopt(argc, argv, "dls:")) != -1; ) {
    switch (opt) {
      case 'd':
        enable_fuse_debug_logging = true;
        break;
      case 'l':
        logging_enabled = true;
        break;
      case 's':
        sqlite_path_for_dump = optarg;
        break;
      default:
        fputs(
          "Usage: tests [-l] [-d] [-s sqlite3] [<tests...>]\n\n"
          "Options:\n"
          "\t-l         enable printing of log statements\n"
          "\t-d         enable printing libfuse debug output\n"
          "\t-s sqlite3 name of the sqlite3 binary (if specified, the contents\n"
          "\t           of the database will be dumped after each test)\n",
          stdout);
        exit(1);
    }
  }

  // Run tests.
  CHECK(optind <= argc);
  global_setup();
  bool all_pass = test_run(tests, (const char**)argv + optind, argc - optind);
  global_teardown();
  return all_pass ? 0 : 1;
}
