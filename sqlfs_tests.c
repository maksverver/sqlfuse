// Unit tests for sqlfs library.
//
// Note: these tests create temporary directories named /tmp/test-xxx-yy. If a
// test fails, it may leave the test directory behind. You'll have to clean it
// up manually.
//
// To use a different directory, set the TMPDIR environmental variable to the
// directory root to use instead of /tmp.

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
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

#include "logging.h"
#include "sqlfs.h"
#include "test_common.h"

// File descriptor for error output.
#define FD_ERR 2

static char *testdir;
static char *database;
static struct sqlfs *sqlfs;

static void global_setup() {
  const char *tempdir = getenv("TMPDIR");
  if (tempdir == NULL || *tempdir == '\0') {
    tempdir = "/tmp";
  }
  testdir = aprintf("%s/test-%d-%06d", tempdir, (int)getpid(), (int)(time(NULL)%1000000));
  database = aprintf("%s/db", testdir);
  CHECK(mkdir(testdir, 0700) == 0);
}

static void global_teardown() {
  CHECK(rmdir(testdir) == 0);
  free(testdir);
  testdir = NULL;
  free(database);
  database = NULL;
}

static struct sqlfs_options get_options(const char *password) {
  const struct sqlfs_options options = {
      .filepath = database,
      .password = password,
      .uid = geteuid(),
      .gid = getegid(),
      .umask = 0022,
      // We set kdf_iter to a very low number, because the default setting makes
      // this test run very slowly when running with leak checking enabled.
      //
      // The reason for this is that OpenSSL does several memory allocations per
      // iteration, so the default setting causes a huge amount allocations. We
      // can get away with lowering the number of iterations during the test,
      // because we don't care about key strength while testing.
      .kdf_iter = 10,
  };
  return options;
}

static void create_database(const char *password) {
  const struct sqlfs_options options = get_options(password);
  CHECK(sqlfs_create(&options) == 0);
}

static struct sqlfs *open_database(const char *password) {
  const struct sqlfs_options options = get_options(password);
  return sqlfs_open(SQLFS_OPEN_MODE_READWRITE, &options);
}

static int suppress_stderr() {
  fflush(stderr);
  int fd_old = dup(FD_ERR);
  int fd_new = open("/dev/null", O_WRONLY);
  CHECK(fd_new >= 0);
  dup2(fd_new, FD_ERR);
  close(fd_new);
  return fd_old;
}

static void restore_stderr(int fd_old) {
  fflush(stderr);
  dup2(fd_old, FD_ERR);
  close(fd_old);
}

// Temporarily suppresses output to stderr while opening a database. This is
// used in tests that expect opening to fail.
static struct sqlfs *open_database_no_errors(const char *password) {
  int fd_old = suppress_stderr();
  struct sqlfs *sqlfs = open_database(password);
  restore_stderr(fd_old);
  return sqlfs;
}

static void setup() {
  const char *password = "test-password-123";
  create_database(password);
  sqlfs = open_database(password);
  CHECK(sqlfs);
}

static void teardown() {
  sqlfs_close(sqlfs);
  sqlfs = NULL;
  CHECK(unlink(database) == 0);
  free_deferred();
}

static void test_basic() {
  setup();
  // Doesn't do anything. Just verifies the test framework works.
  teardown();
}

static void test_with_password() {
  create_database("foo");
  EXPECT(open_database_no_errors(NULL) == NULL);
  EXPECT(open_database_no_errors("bar") == NULL);
  sqlfs = open_database("foo");
  EXPECT(sqlfs);
  teardown();
}

static void test_without_password() {
  create_database(NULL);
  EXPECT(open_database_no_errors("") == NULL);
  sqlfs = open_database(NULL);
  EXPECT(sqlfs);
  teardown();
}

static void verify_contents(ino_t ino, const char *data, size_t size) {
  char buf[100];  // big enough for our tests
  CHECK(size < sizeof(buf));
  size_t nread = 0;
  EXPECT_EQ(sqlfs_read(sqlfs, ino, 0, sizeof(buf), buf, &nread), 0);
  EXPECT_EQ((int)size, (int)nread);
  EXPECT_EQ(memcmp(buf, data, size), 0);
}

static void test_blocksize() {
  setup();

  // Default block size is 4096. test_read() also depends on this.
  EXPECT_EQ(sqlfs_get_blocksize(sqlfs), 4096);

  struct stat attr;
  EXPECT_EQ(sqlfs_mknod(sqlfs, SQLFS_INO_ROOT, "a", S_IFREG | 0600, &attr), 0);
  EXPECT_EQ(attr.st_blksize, 4096);

  sqlfs_set_blocksize(sqlfs, 42);
  EXPECT_EQ(sqlfs_mknod(sqlfs, SQLFS_INO_ROOT, "b", S_IFREG | 0600, &attr), 0);
  EXPECT_EQ(attr.st_blksize, 42);

  EXPECT_EQ(sqlfs_stat_entry(sqlfs, SQLFS_INO_ROOT, "a", &attr), 0);
  EXPECT_EQ(attr.st_blksize, 4096);

  EXPECT_EQ(sqlfs_stat_entry(sqlfs, SQLFS_INO_ROOT, "b", &attr), 0);
  EXPECT_EQ(attr.st_blksize, 42);

  // TODO: would be nice to check that blocksize actually affects how files are
  // stored. For example, set blocksize=10, write a 25 byte file, and check that
  // 3 blocks of size 10, 10 and 5 are stored in the filedata table.

  teardown();
}

static void do_read_test(int blocksize, int filesize, int max_offset) {
  // Override blocksize.
  sqlfs_set_blocksize(sqlfs, blocksize);

  // Generate pseudo-random input data.
  char *input = test_alloc(filesize);
  for (int i = 0; i < filesize; ++i) {
    input[i] = rand();
  }

  // Create test file.
  struct stat attr;
  EXPECT_EQ(sqlfs_mknod(sqlfs, SQLFS_INO_ROOT, "file", S_IFREG | 0600, &attr), 0);
  EXPECT_EQ(attr.st_blksize, blocksize);
  const ino_t ino = attr.st_ino;
  EXPECT_EQ(sqlfs_write(sqlfs, ino, 0, filesize, input), 0);

  // Test reading every range [i:j) for 0 <= i <= j <= max_offset.
  char *output = test_alloc(max_offset);
  for (int i = 0; i <= max_offset; ++i) {
    for (int j = i; j <= max_offset; ++j) {
      memset(output, 0x55, max_offset);
      size_t size_read = -1;
      EXPECT_EQ(sqlfs_read(sqlfs, ino, i, j - i, output, &size_read), 0);
      int expected_size =
          (j > filesize ? filesize : j) -
          (i > filesize ? filesize : i);
      CHECK(expected_size >= 0);
      EXPECT_EQ((int)size_read, expected_size);
      if (expected_size > 0) {
        EXPECT_EQ(memcmp(input + i, output, expected_size), 0);
      }
    }
  }

  // Unlink test file.
  ino_t removed_ino = SQLFS_INO_NONE;
  EXPECT_EQ(sqlfs_unlink(sqlfs, SQLFS_INO_ROOT, "file", &removed_ino), 0);
  EXPECT_EQ(removed_ino, ino);
}

static void test_read() {
  setup();

  // File size is not a multiple of the blocksize.
  do_read_test(6, 50, 60);
  do_read_test(7, 50, 60);
  do_read_test(8, 50, 60);

  // File size is a multiple of the blocksize.
  do_read_test(6, 36, 50);
  do_read_test(7, 35, 50);
  do_read_test(8, 40, 50);

  teardown();
}

// There is a similar test in sqlfuse_internal_tests.
static void test_write_edgecases() {
  // Tricky cases (to unit test!)
  //
  //        0123 4567 8901 2345 6789
  // file: |----|----|--..|....|....|
  //
  //  a.   |....|----|....|  exactly one temp_block
  //  b.   |----|----|....|  exactly two blocks
  //  c.   |---.|....|....|  first half of a block
  //  d.   |.---|....|....|  last half of a block
  //  e.   |.--.|....|....|  middle of a block
  //  f.   |..--|--..|....|  spanning two different blocks
  //  g.   |...-|----|-...|  write mixes whole and partial blocks
  //  h.   |.---|----|---.|  write extends old temp_block
  //  i.   |....|....|....|.-..|....|   write outside range
  //  j.   |....|....|....|...-|--..|   write outside range
  //
  // Note that case e doesn't extend beyond the end of the file, while f does.
  // Note that the open space introduced by g will be zero-filled.

  setup();

  // Small blocksize to make these tests easier to write.
  sqlfs_set_blocksize(sqlfs, 4);
  EXPECT_EQ(sqlfs_get_blocksize(sqlfs), 4);

  struct stat attr;
  EXPECT_EQ(sqlfs_mknod(sqlfs, SQLFS_INO_ROOT, "file", S_IFREG | 0600, &attr), 0);
  const ino_t ino = attr.st_ino;

  EXPECT_EQ(sqlfs_write(sqlfs, ino, 0, 10, "xxxxxxxxxx"), 0);
  verify_contents(ino, "xxxxxxxxxx", 10);

  EXPECT_EQ(sqlfs_write(sqlfs, ino, 4, 4, "aaaa"), 0);
  verify_contents(ino, "xxxxaaaaxx", 10);

  EXPECT_EQ(sqlfs_write(sqlfs, ino, 0, 8, "bbbbbbbb"), 0);
  verify_contents(ino, "bbbbbbbbxx", 10);

  EXPECT_EQ(sqlfs_write(sqlfs, ino, 0, 3, "ccc"), 0);
  verify_contents(ino, "cccbbbbbxx", 10);

  EXPECT_EQ(sqlfs_write(sqlfs, ino, 1, 3, "ddd"), 0);
  verify_contents(ino, "cdddbbbbxx", 10);

  EXPECT_EQ(sqlfs_write(sqlfs, ino, 1, 2, "ee"), 0);
  verify_contents(ino, "ceedbbbbxx", 10);

  EXPECT_EQ(sqlfs_write(sqlfs, ino, 2, 4, "ffff"), 0);
  verify_contents(ino, "ceffffbbxx", 10);

  EXPECT_EQ(sqlfs_write(sqlfs, ino, 3, 6, "gggggg"), 0);
  verify_contents(ino, "cefggggggx", 10);

  EXPECT_EQ(sqlfs_write(sqlfs, ino, 1, 10, "hhhhhhhhhh"), 0);
  verify_contents(ino, "chhhhhhhhhh", 11);

  EXPECT_EQ(sqlfs_write(sqlfs, ino, 13, 1, "i"), 0);
  verify_contents(ino, "chhhhhhhhhh\0\0i", 14);

  EXPECT_EQ(sqlfs_write(sqlfs, ino, 15, 3, "jjj"), 0);
  verify_contents(ino, "chhhhhhhhhh\0\0i\0jjj", 18);

  teardown();
}

static const struct test_case tests[] = {
#define TEST(x) {#x, &test_##x}
  TEST(basic),
  TEST(with_password),
  TEST(without_password),
  TEST(blocksize),
  TEST(read),
  TEST(write_edgecases),
#undef TEST
  {NULL, NULL}};

int main(int argc, char *argv[]) {
  // Parse command line options.
  for (int opt; (opt = getopt(argc, argv, "l")) != -1; ) {
    switch (opt) {
      case 'l':
        logging_enabled = true;
        break;
      default:
        fputs(
          "Usage: sqlfs_tests [-l] [<tests...>]\n\n"
          "Options:\n"
          "\t-l         enable printing of log statements\n",
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
