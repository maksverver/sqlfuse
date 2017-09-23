#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fuse.h"
#include "fuse_lowlevel.h"

#include "intmap.h"
#include "logging.h"
#include "sqlfs.h"
#include "sqlfuse.h"

#ifdef WITH_MTRACE
#include <mcheck.h>
#endif

#define PROGRAM_NAME "sqlfuse"

// Returns the current umask. WARNING: This is not thread-safe!
static mode_t getumask() {
  mode_t mask = umask(0);
  CHECK(umask(mask) == 0);
  return mask;
}

// Overwrites the contents of `password` with zeroes, as a security measure.
// `password` may be NULL, in which case this function does nothing.
static void clear_password(char *password) {
  if (password != NULL) {
    memset(password, 0, strlen(password));
  }
}

static bool finish_daemonization() {
  if (setsid() == -1) {
    perror("setsid()");
    return false;
  }
  if (chdir("/") != 0) {
    perror("chdir(\"/\")");
    return false;
  }
  int fd = open("/dev/null", O_RDWR);
  if (fd == -1) {
    perror("open(\"/dev/null\")");
    return false;
  }
  dup2(fd, 0);
  dup2(fd, 1);
  dup2(fd, 2);
  close(fd);
  return true;
}

// Runs the FUSE low-level main loop. Returns 0 on success.
// Based on: https://github.com/libfuse/libfuse/blob/fuse_2_6_bugfix/example/hello_ll.c
//
// When running in the background, this function is called from the forked child
// process. We don't demonize just before entering the FUSE main loop, because
// we cannot carry SQLite database handles across process boundaries (any locks
// acquired would be lost).
//
// If pipefd is nonnegative, then the function assumes it is running in the
// background. It will finish demonization by calling finish_demonize(), defined
// above, and then writing a 0 byte to pipefd to signal succesful completion.
//
// See run_mount() below, for how this function is used.
static int sqlfuse_main(
    struct fuse_args *fuse_args,
    bool readonly,
    char *password,
    const char *filepath,
    const char *mountpoint,
    int pipefd) {
  const bool foreground = pipefd < 0;
  logging_enabled = foreground;
  enum sqlfs_open_mode open_mode = readonly ? SQLFS_OPEN_MODE_READONLY : SQLFS_OPEN_MODE_READWRITE;
  struct sqlfs *sqlfs = sqlfs_open(filepath, open_mode, password, getumask(), geteuid(), getegid());
  clear_password(password);
  password = NULL;
  if (!sqlfs) {
    fprintf(stderr, "Failed to open database '%s'.\n", filepath);
    return 1;
  }
  if (sqlfs_purge_all(sqlfs) != 0) {
    fprintf(stderr, "Failed to purge unlinked entries in database '%s'.\n", filepath);
    return 1;
  }
  struct intmap *lookups = intmap_create();
  if (lookups == NULL) {
    fprintf(stderr, "Failed to create intmap.\n");
    sqlfs_close(sqlfs);
    return 1;
  }
  int err = -1;
  struct fuse_chan *chan = fuse_mount(mountpoint, fuse_args);
  if (chan != NULL) {
    struct sqlfuse_userdata sqlfuse_userdata = { .sqlfs = sqlfs, .lookups = lookups };
    struct fuse_session *session = fuse_lowlevel_new(
        fuse_args, &sqlfuse_ops, sizeof(sqlfuse_ops), &sqlfuse_userdata);
    if (session != NULL) {
      if (fuse_set_signal_handlers(session) == 0) {
        fuse_session_add_chan(session, chan);
        if (foreground || finish_daemonization()) {
          if (!foreground) {
            // Daemonization complete. Tell the parent process to exit.
            char status = 0;
            if (write(pipefd, &status, sizeof(status)) != 1) {
              perror("write()");
            }
            if (close(pipefd) != 0) {
              perror("close()");
            }
          }
          // Run the actual FUSE session loop.
          err = fuse_session_loop(session);
        }
        fuse_remove_signal_handlers(session);
        fuse_session_remove_chan(chan);
      }
      fuse_session_destroy(session);
    }
    fuse_unmount(mountpoint, chan);
  }
  intmap_destroy(lookups);
  sqlfs_close(sqlfs);
  return err;
}

static char *get_password_with_prompt(const char *prompt) {
  // Note: getpass() allocates a temporary buffer (via getline()) which is never
  // freed. When running with mtrace, this will be reported as a memory leak.
  char *password = getpass(prompt);
  if (password == NULL) {
    fprintf(stderr, "Failed to read password.\n");
    return NULL;
  }
  if (!*password) {
    fprintf(stderr, "Empty password not accepted. (Use the -n/--no_password option to disable encryption.)\n");
    return NULL;
  }
  return password;
}

// Returns a non-empty password in a temporary buffer, or NULL if the password
// could not be read. (If NULL is returned, an appropriate error message has
// been printed to stderr.)
static char *get_password() {
  return get_password_with_prompt("Password: ");
}

// Similar to get_password(), but prompts for the password twice, and verifies
// the same password is entered each time. This is intended to prevent typos
// when setting a new password.
static char *get_new_password() {
  char *copy = NULL;
  char *password = get_password_with_prompt("New password: ");
  if (password == NULL) {
    goto finish;
  }
  copy = strdup(password);
  CHECK(copy);
  password = get_password_with_prompt("New password (again): ");
  if (password == NULL) {
    goto finish;
  }
  if (strcmp(password, copy) != 0) {
    fprintf(stderr, "Passwords do not match.\n");
    clear_password(password);
    password = NULL;
  }
finish:
  clear_password(copy);
  free(copy);
  return password;
}

static bool starts_with(const char *string, const char *prefix) {
  return strncmp(string, prefix, strlen(prefix)) == 0;
}

struct mount_args {
  bool help;
  bool version;
  bool no_password;
  bool readonly;
  bool debug;
  bool have_fsname;
  bool have_subtype;
  const char *filepath;
  char *insecure_password;
};

// Extract arguments for the `mount` command. This is special since it
// recognizes some of the FUSE options, but does not remove them from the
// argument list.
struct mount_args extract_mount_arguments(int *argc, char **argv) {
  struct mount_args args = {
    .help = false,
    .version = false,
    .no_password = false,
    .readonly = false,
    .debug = false,
    .filepath = NULL,
    .insecure_password = NULL };
  const int n = *argc;
  int j = 1;
  assert(n >= 1);
  for (int i = j; i < n; ++i) {
    char *arg = argv[i];
    if (strcmp(arg, "-n") == 0 || strcmp(arg, "--no_password") == 0) {
      args.no_password = true;
    } else if (arg[0] != '-' && args.filepath == NULL) {
      args.filepath = arg;
    } else if (starts_with(arg, "--insecure_password=")) {
      args.insecure_password = strchr(arg, '=') + 1;
    } else {
      // Keep this argument.
      argv[j++] = arg;

      // Parse options which will be passed through to fuse_main:
      //  -h / --help
      //  -V / --version
      //  -d / -odebug / -o debug
      //  -oro / -o ro
      //  -ofsname=... / -o fsname=...
      //  -osubtype=... / -o subtype=...
      if (strcmp(arg, "-o") == 0) {
        if (i + 1 < n) {
          arg = argv[++i];
          argv[j++] = arg;
          if (strcmp(arg, "debug") == 0) {
            args.debug = true;
          } else if (strcmp(arg, "ro") == 0) {
            args.readonly = true;
          } else if (starts_with(arg, "fsname=")) {
            args.have_fsname = true;
          } else if (starts_with(arg, "subtype=")) {
            args.have_subtype = true;
          }
        }
      } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
        args.help = true;
      } else if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
        args.version = true;
      } else if (strcmp(arg, "-d") == 0 || strcmp(arg, "-odebug") == 0) {
        args.debug = true;
      } else if (strcmp(arg, "-oro") == 0) {
        args.readonly = true;
      } else if (starts_with(arg, "-ofsname=")) {
        args.have_fsname = true;
      } else if (starts_with(arg, "-osubtype=")) {
        args.have_subtype = true;
      }
    }
  }
  *argc = j;
  argv[j] = NULL;
  return args;
}

struct args {
  bool no_password;
  char *insecure_password;
  char *old_insecure_password;
  char *new_insecure_password;
};

enum {
  ARG_NO_PASSWORD = 1,
  ARG_PLAINTEXT_PASSWORD = 2,
  ARG_OLD_PLAINTEXT_PASSWORD = 4,
  ARG_NEW_PLAINTEXT_PASSWORD = 8 };

// Parses and removes recognized options from the given argument list.
//
// supported_args is a bitmask of arguments to be recognized.
static bool parse_args(int *argc, char *argv[], int supported_args, struct args *args) {
  int j = 1;
  bool success = true;
  bool no_more_options = false;
  int set = 0;
  for (int i = 1; i < *argc; ++i) {
    char *arg = argv[i];
    if (*arg == '-' && !no_more_options) {
      int recognized = 0;
      if (strcmp(arg, "--") == 0) {
        no_more_options = true;
      } else if ((supported_args & ARG_NO_PASSWORD) &&
          (strcmp(arg, "-n") == 0 || strcmp(arg, "--no_password") == 0)) {
        args->no_password = true;
        recognized = ARG_NO_PASSWORD;
      } else if ((supported_args & ARG_PLAINTEXT_PASSWORD) &&
            starts_with(arg, "--insecure_password=")) {
        args->insecure_password = strchr(arg, '=') + 1;
        recognized = ARG_PLAINTEXT_PASSWORD;
      } else if ((supported_args & ARG_OLD_PLAINTEXT_PASSWORD) &&
            starts_with(arg, "--old_insecure_password=")) {
        args->old_insecure_password = strchr(arg, '=') + 1;
        recognized = ARG_OLD_PLAINTEXT_PASSWORD;
      } else if ((supported_args & ARG_NEW_PLAINTEXT_PASSWORD) &&
            starts_with(arg, "--new_insecure_password=")) {
        args->new_insecure_password = strchr(arg, '=') + 1;
        recognized = ARG_NEW_PLAINTEXT_PASSWORD;
      }
      if (recognized) {
        if (set & recognized) {
          fprintf(stderr, "Option argument specified more than once: %s\n", arg);
          success = false;
        } else {
          set |= recognized;
        }
      } else {
        fprintf(stderr, "Unrecognized option argument: %s\n", arg);
        success = false;
      }
    } else {
      argv[j++] = arg;
    }
  }
  *argc = j;
  return success;
}

static bool validate_database_path(const char *path, bool should_exist) {
  struct stat st;
  if (stat(path, &st) != 0) {
    if (errno != ENOENT) {
      perror(NULL);
      return false;
    }
    if (should_exist) {
      fprintf(stderr, "Database '%s' does not exist.\n", path);
      return false;
    }
  } else {
    if (!S_ISREG(st.st_mode)) {
      fprintf(stderr, "Database '%s' is not a regular file.\n", path);
      return false;
    }
    if (!should_exist) {
      fprintf(stderr, "Database '%s' already exists.\n", path);
      return false;
    }
  }
  return true;
}

// If the only remaining argument is the database filepath, it is validated and
// returned. In case of an error, an appropriate error message is printed and
// NULL is returned, instead.
static const char *get_database_argument(int argc, char *argv[], bool should_exist) {
  if (argc < 2) {
    fprintf(stderr, "Missing argument: database path.\n");
    return NULL;
  }
  if (argc > 2) {
    fprintf(stderr, "Unexpected arguments after database path.\n");
    return NULL;
  }
  const char *path = argv[1];
  if (!validate_database_path(path, should_exist)) {
    return NULL;
  }
  return path;
}

static char *delete_arg(int index, int *argc, char *argv[]) {
  int n = --*argc;
  if (index > n) {
    return NULL;
  }
  char *result = argv[index];
  for (int i = index; i < n; ++i) {
    argv[i] = argv[i + 1];
  }
  argv[n] = NULL;
  return result;
}

struct fuse_args alloc_fuse_args(int argc, char *argv[]) {
  struct fuse_args result = {
    .argc = argc,
    .argv = calloc(argc + 1, sizeof(char*)),
    .allocated = 1};
  CHECK(result.argv);
  for (int i = 0; i < argc; ++i) {
    result.argv[i] = strdup(argv[i]);
  }
  return result;
}

static void print_version() {
  printf("%s version %d.%d.%d (database version %d)\n",
      PROGRAM_NAME, SQLFUSE_VERSION_MAJOR, SQLFUSE_VERSION_MINOR, SQLFUSE_VERSION_PATCH, SQLFS_SCHEMA_VERSION);
}

static int run_help() {
  print_version();
  fputs("\nUsage:\n"
      "    sqlfuse help\n"
      "    sqlfuse create [-n|--no_password] <database>\n"
      "    sqlfuse mount [options] <database> <mountpoint> [FUSE options]\n"
      "    sqlfuse rekey <database>\n"
      "    sqlfuse compact [-n|--no_password] <database>\n"
      "    sqlfuse check [-n|--no_password] <database>\n"
      "\n"
      "Mount options:\n"
      "    -n|--no_password  Create or open an unencrypted database.\n"
      "    -h|--help         Verbose help (including FUSE mount options).\n",
      stdout);
  return 0;
}

static int run_create(int argc, char *argv[]) {
  struct args args = {0};
  if (!parse_args(&argc, argv, ARG_NO_PASSWORD | ARG_PLAINTEXT_PASSWORD, &args)) {
    return 1;
  }
  const char *database = get_database_argument(argc, argv, false /* should_exist */);
  if (database == NULL) {
    return 1;
  }
  char *password = NULL;
  if (!args.no_password) {
    password = args.insecure_password ? args.insecure_password : get_new_password();
    if (password == NULL) {
      return 1;
    }
  }
  int err = sqlfs_create(database, password, getumask(), geteuid(), getegid());
  clear_password(password);
  password = NULL;
  if (err != 0) {
    fprintf(stderr, "Failed to create database '%s'.\n", database);
    return 1;
  }
  printf("Created database '%s' (%s)\n", database, args.no_password ? "not encrypted" : "encrypted");
  return 0;
}

static int run_mount(int argc, char *argv[]) {
  struct mount_args args = extract_mount_arguments(&argc, argv);

  sqlfuse_tracing_enabled = args.debug;

  if (args.help || args.version) {
    if (args.version) {
      print_version();
    }
    if (args.help) {
      fputs(
          "Usage: sqlfuse mount [options] <database> <mountpoint> [fuse options]\n"
          "\n"
          "Supported options:\n"
          "    -n   --no_password    don't prompt for password (disables encryption)\n"
          "\n",
          stdout);
    }

    // BUG: this prints usage for the high-level FUSE API, while we're really
    // using the low-level API. TODO: fix this somehow?
    fuse_main(argc, argv, (const struct fuse_operations*)NULL, NULL);
    return 0;
  }

  if (args.filepath == NULL) {
    fprintf(stderr, "Missing database filename.\n");
    return 1;
  }

  if (!validate_database_path(args.filepath, true)) {
    return 1;
  }

  char *password = NULL;
  if (!args.no_password) {
    password = args.insecure_password ? args.insecure_password : get_password();
    if (password == NULL) {
      return 1;
    }
  }

  struct fuse_args fuse_args = alloc_fuse_args(argc, argv);
  if (!args.have_fsname) {
    // Add the absolute path to the database file as the filesystem name.
    // This shows up as the first field in mount.
    char abspath[PATH_MAX];
    if (realpath(args.filepath, abspath) == NULL) {
      // If we couldn't resolve the absolute path for whatever reason, keep the
      // relative path, instead.
      snprintf(abspath, sizeof(abspath), "%s", args.filepath);
    }
    char arg[PATH_MAX + 16];
    snprintf(arg, sizeof(arg), "-ofsname=%s", abspath);
    fuse_opt_add_arg(&fuse_args, arg);
  }
  if (!args.have_subtype) {
    // Set the filesystem subtype. This shows up as the third field in mount.
    fuse_opt_add_arg(&fuse_args, "-osubtype=" PROGRAM_NAME);
  }

  char *mountpoint = NULL;
  int multithreaded = 0;
  int foreground = 0;
  if (fuse_parse_cmdline(&fuse_args, &mountpoint, &multithreaded, &foreground) != 0) {
    free(mountpoint);
    fuse_opt_free_args(&fuse_args);
    return 1;
  }

  // `multithreaded` is ignored, because we only support single-threaded mode.
  // We used to print a warning, but it's confusing and annoying, so silently
  // run in single-threaded mode instead.

  if (foreground) {
    // Foreground mode. Run sqlfuse_main() directly, without forking.
    int err = sqlfuse_main(&fuse_args, args.readonly, password, args.filepath, mountpoint, -1);
    free(mountpoint);
    fuse_opt_free_args(&fuse_args);
    return err ? 1 : 0;
  }

  // Background mode. We will create a pipe for synchronization: the child
  // process will write a status byte when daemonization is complete, and the
  // parent process does a blocking read to wait for it.
  int pipefds[2];
  if (pipe(pipefds) == -1) {
    perror("pipe()");
    return -1;
  }
  pid_t child_pid = fork();
  if (child_pid == -1) {
    perror("fork()");
    close(pipefds[0]);
    close(pipefds[1]);
    return -1;
  }
  if (child_pid == 0) {
    // In the child process.
    close(pipefds[0]);
    int err = sqlfuse_main(&fuse_args, args.readonly, password, args.filepath, mountpoint, pipefds[1]);
    free(mountpoint);
    fuse_opt_free_args(&fuse_args);
    exit(err ? 1 : 0);
    return 1;  // unreachable, because exit() does not return
  } else {
    // In the parent process.
    close(pipefds[1]);
    clear_password(password);
    free(mountpoint);
    fuse_opt_free_args(&fuse_args);
    // Wait for demonization. If read() fails, that means the child exited
    // without writing anything to the pipe, which means something went wrong.
    char status;
    if (read(pipefds[0], &status, sizeof(status)) != sizeof(status)) {
      status = 1;
    }
    close(pipefds[0]);
    return status;
  }
}

static int run_rekey(int argc, char *argv[]) {
  struct args args = {0};
  if (!parse_args(&argc, argv, ARG_OLD_PLAINTEXT_PASSWORD | ARG_NEW_PLAINTEXT_PASSWORD, &args)) {
    return 1;
  }
  const char *database = get_database_argument(argc, argv, true /* must_exist */);
  if (database == NULL) {
    return 1;
  }

  int result = 1;  // exit failure
  struct sqlfs *sqlfs = NULL;
  char *old_password = NULL;
  char *old_password_copy = NULL;
  char *new_password = NULL;

  old_password = args.old_insecure_password ? args.old_insecure_password : get_password();
  if (old_password == NULL) {
    goto finish;
  }
  sqlfs = sqlfs_open(database, SQLFS_OPEN_MODE_READWRITE, old_password, getumask(), geteuid(), getegid());
  if (sqlfs == NULL) {
    fprintf(stderr, "Failed to open database '%s'.\n", database);
    goto finish;
  }

  // Since the pointer returned by getpass() is only valid until the next call
  // to getpass(), we need to copy old_password here.
  old_password_copy = strdup(old_password);
  CHECK(old_password_copy != NULL);
  clear_password(old_password);
  old_password = old_password_copy;

  new_password = args.new_insecure_password ? args.new_insecure_password :  get_new_password();
  if (new_password == NULL) {
    goto finish;
  }
  if (strcmp(old_password, new_password) == 0) {
    printf("Password unchanged.\n");
  } else {
    if (sqlfs_rekey(sqlfs, new_password) != 0) {
      fprintf(stderr, "Could not rekey database '%s' (not writable?)\n", database);
      goto finish;
    }
    printf("Password changed.\n");
  }
  result = 0;  // exit successfully

finish:
  if (sqlfs != NULL) {
    sqlfs_close(sqlfs);
  }
  clear_password(old_password);
  clear_password(new_password);
  free(old_password_copy);
  return result;
}

static struct sqlfs *open_sqlfs_from_args(int argc, char *argv[], enum sqlfs_open_mode open_mode) {
  struct args args = {0};
  if (!parse_args(&argc, argv, ARG_NO_PASSWORD | ARG_PLAINTEXT_PASSWORD, &args)) {
    return NULL;
  }
  const char *database = get_database_argument(argc, argv, true /* should_exist */);
  if (database == NULL) {
    return NULL;
  }
  char *password = NULL;
  if (!args.no_password) {
    password = args.insecure_password ? args.insecure_password : get_password();
    if (password == NULL) {
      return NULL;
    }
  }
  return sqlfs_open(database, open_mode, password, getumask(), geteuid(), getegid());
}

static int run_compact(int argc, char *argv[]) {
  struct sqlfs *sqlfs = open_sqlfs_from_args(argc, argv, SQLFS_OPEN_MODE_READWRITE);
  if (sqlfs == NULL) {
    return 1;
  }
  int status = 1;  // exit failure
  if (sqlfs_purge_all(sqlfs) != 0) {
    fprintf(stderr, "Failed to purge unlinked files/directories.\n");
  } else if (sqlfs_vacuum(sqlfs) != 0) {
    fprintf(stderr, "Failed to vacuum the database.\n");
  } else {
    printf("Database compaction complete.\n");
    status = 0;  // exit successfully
  }
  sqlfs_close(sqlfs);
  return status;
}

static int run_check(int argc, char *argv[]) {
  struct sqlfs *sqlfs = open_sqlfs_from_args(argc, argv, SQLFS_OPEN_MODE_READONLY);
  if (sqlfs == NULL) {
    return 1;
  }
  sqlfs_close(sqlfs);
  // TODO: implement this!
  fprintf(stderr, "Command 'check' not yet implemented!\n");
  return 1;
}

int main(int argc, char *argv[]) {
#ifdef WITH_MTRACE
  mtrace();
#endif

  const char *command = argc < 2 ? "help" : delete_arg(1, &argc, argv);

  if (strcmp(command, "create") == 0) {
    return run_create(argc, argv);
  }

  if (strcmp(command, "mount") == 0) {
    return run_mount(argc, argv);
  }

  if (strcmp(command, "rekey") == 0) {
    return run_rekey(argc, argv);
  }

  if (strcmp(command, "compact") == 0) {
    return run_compact(argc, argv);
  }

  if (strcmp(command, "check") == 0) {
    return run_check(argc, argv);
  }

  if (strcmp(command, "help") == 0) {
    return run_help();
  }

  run_help();
  fprintf(stderr, "\nUnsupported command: '%s'\n", command);
  return 1;
}
