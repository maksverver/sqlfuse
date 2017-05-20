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
#include <dirent.h>
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

// Linked list node for a deferred allocation. (See defer_free().)
struct allocation {
  struct allocation *next;
  void *ptr;
};

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
static struct allocation *allocations;

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

// Stores ptr as an allocation to be freed later by a call to free_deferred().
static void defer_free(char *ptr) {
  struct allocation *alloc = calloc(1, sizeof(struct allocation));
  CHECK(alloc);
  alloc->next = allocations;
  alloc->ptr = ptr;
  allocations = alloc;
}

// Frees all allocations passed to defer_free() before.
static void free_deferred() {
  struct allocation *alloc;
  while ((alloc = allocations) != NULL) {
    allocations = alloc->next;
    free(alloc->ptr);
    free(alloc);
  }
}

// Returns a temporary string formed as: mountpoint + "/" + relpath.
// The string should NOT be freed by the caller.
static char *makepath(const char *relpath) {
  char *path = aprintf("%s/%s", mountpoint, relpath);
  defer_free(path);
  return path;
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

  free_deferred();
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
  setup();

  EXPECT_EQ(nlinks(mountpoint), 2);

  char *subdir = makepath("subdir");
  EXPECT_EQ(mkdir(subdir, 0770), 0);
  struct stat st = {0};
  EXPECT_EQ(stat(subdir, &st), 0);
  EXPECT_EQ(st.st_mode, 0750 | S_IFDIR);  // umask has been applied
  EXPECT_EQ(st.st_nlink, 2);

  // Root directory link count has increased.
  EXPECT_EQ(nlinks(mountpoint), 3);

  // Cannot recreate directory that already exists.
  EXPECT_EQ(mkdir(subdir, 0755), -1);
  EXPECT_EQ(errno, EEXIST);

  // Cannot create child directory in non-existent parent directory
  EXPECT_EQ(mkdir(makepath("nonexistent/subdir"), 0770), -1);
  EXPECT_EQ(errno, ENOENT);

  // TODO: Maybe add test for mkdirat? What happens if parent dir is already deleted?

  teardown();
}

static void test_rmdir() {
  struct stat st;

  setup();

  char *path_foo = makepath("foo");
  char *path_foo_bar = makepath("foo/bar");
  char *path_foo_baz = makepath("foo/baz");

  EXPECT_EQ(rmdir(path_foo), -1);
  EXPECT_EQ(errno, ENOENT);

  EXPECT_EQ(rmdir(path_foo_bar), -1);
  EXPECT_EQ(errno, ENOENT);

  EXPECT_EQ(mkdir(path_foo, 0700), 0);
  EXPECT_EQ(mkdir(path_foo_bar, 0700), 0);
  EXPECT_EQ(mkdir(path_foo_baz, 0700), 0);

  EXPECT_EQ(nlinks(mountpoint),  3); // ".", "..", "foo"
  EXPECT_EQ(nlinks(path_foo),     4); // "../foo", ".", "bar/..", "baz/.."
  EXPECT_EQ(nlinks(path_foo_bar), 2); // ".", "../bar"
  EXPECT_EQ(nlinks(path_foo_bar), 2); // ".", "../baz"

  EXPECT_EQ(rmdir(path_foo), -1);
  EXPECT_EQ(errno, ENOTEMPTY);

  EXPECT_EQ(rmdir(path_foo_bar), 0);
  EXPECT_EQ(stat(path_foo_bar, &st), -1);
  EXPECT_EQ(errno, ENOENT);

  EXPECT_EQ(nlinks(path_foo), 3);
  EXPECT_EQ(rmdir(path_foo), -1);
  EXPECT_EQ(errno, ENOTEMPTY);

  EXPECT_EQ(rmdir(path_foo_baz), 0);
  EXPECT_EQ(stat(path_foo_baz, &st), -1);
  EXPECT_EQ(errno, ENOENT);

  // Cannot rmdir() a file. Use unlink() instead.
  EXPECT_EQ(mknod(path_foo_bar, S_IFREG | 0600, 0), 0);
  EXPECT_EQ(rmdir(path_foo_bar), -1);
  EXPECT_EQ(errno, ENOTDIR);

  // Files do not increase parent directory's hardlink count...
  EXPECT_EQ(nlinks(path_foo), 2);

  // ... but they do prevent them from being deleted.
  EXPECT_EQ(rmdir(path_foo), -1);
  EXPECT_EQ(errno, ENOTEMPTY);

  EXPECT_EQ(unlink(path_foo_bar), 0);
  EXPECT_EQ(nlinks(path_foo), 2);
  EXPECT_EQ(rmdir(path_foo), 0);

  EXPECT_EQ(nlinks(mountpoint), 2);

  teardown();
}

static void test_mknod_unlink() {
  struct stat st;

  setup();

  const char *const path_foo = makepath("foo");
  const char *const path_bar = makepath("bar");
  const char *const path_baz = makepath("baz");

  EXPECT_EQ(mknod(path_foo, S_IFREG | 0644, 0), 0);
  EXPECT_EQ(stat(path_foo, &st), 0);
  EXPECT_EQ(st.st_ino, 2);
  EXPECT_EQ(st.st_mode, S_IFREG | 0644);
  EXPECT_EQ(st.st_nlink, 1);
  EXPECT_EQ(st.st_size, 0);
  EXPECT(st.st_blksize > 0);
  EXPECT_EQ(st.st_blocks, 0);

  EXPECT_EQ(mknod(path_foo, S_IFREG | 0644, 0), -1);
  EXPECT_EQ(errno, EEXIST);

  // Invalid mode (symlinks not supportedy yet).
  EXPECT_EQ(mknod(path_foo, S_IFLNK | 0644, 0), -1);
  EXPECT_EQ(errno, EINVAL);

  EXPECT_EQ(mknod(makepath("foo/bar"), S_IFREG | 0644, 0), -1);
  EXPECT_EQ(errno, ENOTDIR);

  EXPECT_EQ(mknod(makepath("nonexistent/foo"), S_IFREG | 0644, 0), -1);
  EXPECT_EQ(errno, ENOENT);

  EXPECT_EQ(mknod(makepath("."), S_IFREG | 0644, 0), -1);
  EXPECT_EQ(errno, EEXIST);

  EXPECT_EQ(mknod(path_bar, S_IFREG | 0777, 0), 0);
  EXPECT_EQ(stat(path_bar, &st), 0);
  EXPECT_EQ(st.st_ino, 3);
  EXPECT_EQ(st.st_mode, S_IFREG | 0755);  // umask was applied
  EXPECT_EQ(st.st_nlink, 1);

  EXPECT_EQ(unlink(path_foo), 0);
  EXPECT_EQ(stat(path_foo, &st), -1);
  EXPECT_EQ(errno, ENOENT);

  EXPECT_EQ(unlink(path_foo), -1);
  EXPECT_EQ(errno, ENOENT);

  // Can't unlink a directory. Use rmdir() instead.
  EXPECT_EQ(mkdir(path_foo, 0775), 0);
  EXPECT_EQ(unlink(path_foo), -1);
  EXPECT_EQ(errno, EISDIR);
  EXPECT_EQ(rmdir(path_foo), 0);

  EXPECT_EQ(unlink(path_bar), 0);

  // After files are deleted, their inode numbers may be re-used. Note: the fact
  // that this actually happens should be considered an implementation detail.
  EXPECT_EQ(mknod(path_baz, S_IFREG | 0600, 0), 0);
  EXPECT_EQ(stat(path_baz, &st), 0);
  EXPECT_EQ(st.st_ino, 2);  // foo's old inode number
  EXPECT_EQ(st.st_nlink, 1);

  // TODO: readdir to verify directory contains /bar, /baz
  // TODO: Maybe add test for mknodat? What happens if parent dir is already deleted?
  // TODO: Maybe add test for unlinkat? What happens if parent dir is already deleted?

  teardown();
}

static void verify_directory_contents(const char *relpath, struct dirent expected_entries[], int expected_size) {
  DIR *dir = opendir(makepath(relpath));
  struct dirent de, *de_ptr = NULL;
  if (dir == NULL) {
    fprintf(stderr, "Couldn't open directory [%s]!\n", relpath);
    test_fail();
    return;
  }
  for (int i = 0; i < expected_size; ++i) {
    EXPECT_EQ(readdir_r(dir, &de, &de_ptr), 0);
    if (de_ptr == NULL) {
      fprintf(stderr, "Premature end of directory [%s] (after %d entries)\n", relpath, i);
      closedir(dir);
      return;
    }
    EXPECT(de_ptr == &de);
    if (expected_entries[i].d_ino != de.d_ino) {
      fprintf(stderr, "Entry %d in %s: expected ino %lld, read ino %lld\n",
          i, relpath, (long long)expected_entries[i].d_ino, (long long)de.d_ino);
      test_fail();
    }
    if (strcmp(expected_entries[i].d_name, de.d_name) != 0) {
      fprintf(stderr, "Entry %d in %s: expected name [%s], read name [%s]\n",
          i, relpath, expected_entries[i].d_name, de.d_name);
      test_fail();
    }
    if (expected_entries[i].d_type != de.d_type) {
      fprintf(stderr, "Entry %d in %s: expected type %d, read type %d\n",
          i, relpath, (int)expected_entries[i].d_type, (int)de.d_type);
      test_fail();
    }
  }
  EXPECT_EQ(readdir_r(dir, &de, &de_ptr), 0);
  EXPECT(de_ptr == NULL);
  closedir(dir);
}

static void test_readdir() {
  setup();

  // ino path
  //  1  /
  //  2  /dir
  //  3  /dir/sub
  //  4  /dir/file1
  //  5  /dir/file2
  //  6  /dir/.-

  mkdir(makepath("dir"), 0700);
  mkdir(makepath("dir/sub"), 0700);
  mknod(makepath("dir/file1"), 0600, 0);
  mknod(makepath("dir/file2"), 0600, 0);
  mknod(makepath("dir/.-"), 0600, 0);

  EXPECT(opendir(makepath("nonexistent")) == NULL);
  EXPECT_EQ(errno, ENOENT);

  // read root
  struct dirent root_entries[3] = {
    { .d_ino = 1, .d_name = ".", .d_type = DT_DIR },
    { .d_ino = 1, .d_name = "..", .d_type = DT_DIR },
    { .d_ino = 2, .d_name = "dir", .d_type = DT_DIR }};
  verify_directory_contents("", root_entries, 3);
  verify_directory_contents(".", root_entries, 3);

  // read empty subdir
  struct dirent sub_entries[2] = {
    { .d_ino = 3, .d_name = ".", .d_type = DT_DIR },
    { .d_ino = 2, .d_name = "..", .d_type = DT_DIR }};
  verify_directory_contents("dir/sub", sub_entries, 2);

  // read non-empty dir
  struct dirent dir_entries[6] = {
    { .d_ino = 2, .d_name = ".", .d_type = DT_DIR },
    { .d_ino = 1, .d_name = "..", .d_type = DT_DIR },
    // Note that ".-" is lexicographically less tha "..", but "." and ".." are
    // always the first and second entry.
    { .d_ino = 6, .d_name = ".-", .d_type = DT_REG },
    { .d_ino = 4, .d_name = "file1", .d_type = DT_REG },
    { .d_ino = 5, .d_name = "file2", .d_type = DT_REG },
    { .d_ino = 3, .d_name = "sub", .d_type = DT_DIR }};
  verify_directory_contents("dir", dir_entries, 6);

  // TODO: add a test for fdopendir?

  teardown();
}

static const struct test_case tests[] = {
#define TEST(x) {#x, &test_##x}
  TEST(basic),
  TEST(rootdir),
  TEST(mkdir),
  TEST(rmdir),
  TEST(mknod_unlink),
  TEST(readdir),
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
