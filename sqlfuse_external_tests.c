// Unit tests for sqlfuse, which run the sqlfuse code in a separate process.
//
// These tests allow closing and reopening the same database file.
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
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fuse.h"
#include "fuse_lowlevel.h"

#include "intmap.h"
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
static pid_t fuse_pid;

// Returns a temporary string formed as: mountpoint + "/" + relpath.
// The string should NOT be freed by the caller.
static char *makepath(const char *relpath) {
  char *path = aprintf("%s/%s", mountpoint, relpath);
  defer_free(path);
  return path;
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

static struct sqlfs_options get_options() {
  const struct sqlfs_options options = {
      .filepath = database,
      .password = NULL,
      .uid = geteuid(),
      .gid = getegid(),
      .umask = 0022};
  return options;
}

static void create_database() {
  const struct sqlfs_options options = get_options();
  CHECK(sqlfs_create(&options) == 0);
}

static void mount_sqlfuse(int sqlfs_open_mode) {
  // Create a pipe which will be used to communicate between the processes.
  int pipefd[2];
  CHECK(pipe(pipefd) == 0);

  // Fork off the child process.
  fuse_pid = fork();
  CHECK(fuse_pid >= 0);
  if (fuse_pid == 0) {
    close(pipefd[0]);

    const struct sqlfs_options options = get_options();
    struct sqlfs *sqlfs = sqlfs_open(sqlfs_open_mode, &options);
    CHECK(sqlfs);

    struct intmap *lookups = intmap_create();
    CHECK(lookups);

    struct sqlfuse_userdata sqlfuse_userdata = {.sqlfs = sqlfs, .lookups = lookups};

    char *argv[2] = {"test", NULL};
    struct fuse_args fuse_args;
    memset(&fuse_args, 0, sizeof(fuse_args));
    fuse_args.argc = 1;
    fuse_args.argv = argv;
    fuse_opt_parse(&fuse_args, NULL, NULL, NULL);
    if (enable_fuse_debug_logging) {
      fuse_opt_add_arg(&fuse_args, "-d");
    }
    if (sqlfs_open_mode == SQLFS_OPEN_MODE_READONLY) {
      fuse_opt_add_arg(&fuse_args, "-oro");
    }
    struct fuse_chan *fuse_chan = fuse_mount(mountpoint, &fuse_args);
    CHECK(fuse_chan);
    struct fuse_session *fuse_session =
        fuse_lowlevel_new(&fuse_args, &sqlfuse_ops, sizeof(sqlfuse_ops), &sqlfuse_userdata);
    CHECK(fuse_session);
    fuse_session_add_chan(fuse_session, fuse_chan);
    fuse_set_signal_handlers(fuse_session);

    // Signal initialization complete by closing the pipe.
    close(pipefd[1]);

    int res = fuse_session_loop(fuse_session);

    fuse_remove_signal_handlers(fuse_session);
    fuse_session_remove_chan(fuse_chan);
    fuse_session_destroy(fuse_session);
    fuse_unmount(mountpoint, fuse_chan);
    fuse_opt_free_args(&fuse_args);

    sqlfs_close(sqlfs);

    CHECK(intmap_size(lookups) == 0);
    intmap_destroy(lookups);

    exit(res);
  } else {
    close(pipefd[1]);
    // Wait for child to finish initialization with a blocking read on the pipe.
    char dummy;
    CHECK(read(pipefd[0], &dummy, 1) == 0);
    close(pipefd[0]);
  }
}

static void unmount_sqlfuse() {
  kill(fuse_pid, SIGHUP);

  int status = -1;
  CHECK(waitpid(fuse_pid, &status, 0) == fuse_pid);
  EXPECT_EQ(status, 0);

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
}

static void setup() {
  create_database();
  mount_sqlfuse(SQLFS_OPEN_MODE_READWRITE);
}

static void teardown() {
  unmount_sqlfuse();
  CHECK(unlink(database) == 0);
  free_deferred();
}

static void reopen_readonly() {
  unmount_sqlfuse();
  chmod(database, 0400);
  mount_sqlfuse(SQLFS_OPEN_MODE_READONLY);
}

static void update_contents(const char *path, const char *data, size_t size) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  EXPECT(fd >= 0);
  EXPECT_EQ(write(fd, data, size), size);
  close(fd);
}

static void verify_contents(const char *path, const char *expected_data, int expected_size) {
  struct stat attr;
  if (stat(path, &attr) != 0) {
    perror(path);
    test_fail();
    return;
  }
  EXPECT_EQ(expected_size, attr.st_size);
  if (expected_size != attr.st_size) {
    return;
  }
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    perror(path);
    test_fail();
    return;
  }
  // Read 1 extra byte to detect EOF.
  char *data = malloc(expected_size + 1);
  CHECK(data);
  ssize_t nread = read(fd, data, expected_size + 1);
  EXPECT_EQ(nread, expected_size);
  if (nread == expected_size) {
    for (int i = 0; i < expected_size; ++i) {
      if (expected_data[i] != data[i]) {
        fprintf(stderr, "Mismatch at byte %d: expected 0x%02x, received 0x%02x.\n",
          (int)i, expected_data[i] & 0xff, data[i] & 0xff);
        test_fail();
        break;
      }
    }
  }
  close(fd);
  free(data);
}

static void test_basic() {
  setup();
  teardown();
}

static void test_open_readonly() {
  setup();

  update_contents(makepath("file"), "foo", 3);

  // Can't write to a file that's opened in readonly mode.
  int fd = open(makepath("file"), O_RDONLY, 0666);
  EXPECT(fd >= 0);
  EXPECT_EQ(write(fd, "bar", 3), -1);
  EXPECT_EQ(errno, EBADF);
  close(fd);

  // Reopen the database in read-only mode.
  reopen_readonly();

  // Can't create new file.
  EXPECT_EQ(mknod(makepath("newfile"), 0600, 0), -1);
  EXPECT_EQ(errno, EROFS);

  // Can't create new directory.
  EXPECT_EQ(mkdir(makepath("newdir"), 0755), -1);
  EXPECT_EQ(errno, EROFS);

  // Can't open a file for writing.
  fd = open(makepath("file"), O_RDWR);
  EXPECT(fd < 0);
  EXPECT_EQ(errno, EROFS);
  close(fd);

  // Can't write to a file that's opened in readonly mode.
  fd = open(makepath("file"), O_RDONLY);
  EXPECT(fd >= 0);
  EXPECT_EQ(write(fd, "bar", 3), -1);
  EXPECT_EQ(errno, EBADF);
  close(fd);

  // Can still read content.
  verify_contents(makepath("file"), "foo", 3);

  teardown();
}

static const struct test_case tests[] = {
#define TEST(x) {#x, &test_##x}
  TEST(basic),
  TEST(open_readonly),
#undef TEST
  {NULL, NULL}};

int main(int argc, char *argv[]) {
  // Parse command line options.
  for (int opt; (opt = getopt(argc, argv, "ltds:")) != -1; ) {
    switch (opt) {
      case 'l':
        logging_enabled = true;
        break;
      case 't':
        sqlfuse_tracing_enabled = true;
        break;
      case 'd':
        enable_fuse_debug_logging = true;
        break;
      case 's':
        sqlite_path_for_dump = optarg;
        break;
      default:
        fputs(
          "Usage: sqlfuse_external_tests [-l] [-t] [-d] [-s sqlite3] [<tests...>]\n\n"
          "Options:\n"
          "\t-l         enable printing of log statements\n"
          "\t-t         enable printing of function call traces\n"
          "\t-d         enable printing libfuse debug output\n"
          "\t-s sqlite3 name of the sqlite3 binary (if specified, the contents\n"
          "\t           of the database will be dumped after each test)\n",
          stdout);
        exit(1);
    }
  }
  CHECK(optind <= argc);

  // Run tests.
  TEST_MTRACE();
  global_setup();
  bool all_pass = test_run(tests, (const char**)argv + optind, argc - optind);
  global_teardown();
  return all_pass ? 0 : 1;
}
