#include <assert.h>
#include <errno.h>
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

// Returns the current umask. WARNING: This is not thread-safe!
static mode_t getumask() {
  mode_t mask = umask(0);
  CHECK(umask(mask) == 0);
  return mask;
}

// Runs the FUSE low-level main loop. Returns 0 on success.
// Based on: https://github.com/libfuse/libfuse/blob/fuse_2_6_bugfix/example/hello_ll.c
static int sqlfuse_main(int argc, char* argv[], struct sqlfs *sqlfs, struct intmap *lookups) {
  int err = -1;
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  char *mountpoint = NULL;
  int multithreaded = 0;
  int foreground = 0;

  if (fuse_parse_cmdline(&args, &mountpoint, &multithreaded, &foreground) == 0) {
    if (multithreaded) {
      fprintf(stderr, "WARNING! Multi-threading not supported; running single-threaded. (Pass -s to suppress this warning.)\n");
    }
    logging_enabled = foreground != 0;

    struct fuse_chan *chan = fuse_mount(mountpoint, &args);
    if (chan != NULL) {
      struct sqlfuse_userdata sqlfuse_userdata = { .sqlfs = sqlfs, .lookups = lookups };
      struct fuse_session *session = fuse_lowlevel_new(
          &args, &sqlfuse_ops, sizeof(sqlfuse_ops), &sqlfuse_userdata);
      if (session != NULL) {

        // Daemonization happens here! Afterwards, the cwd will be / and output
        // is redirected to /dev/null. For debugging, run with -d or -f.
        fuse_daemonize(foreground);

        if (fuse_set_signal_handlers(session) == 0) {
          fuse_session_add_chan(session, chan);
          err = fuse_session_loop(session);
          fuse_remove_signal_handlers(session);
          fuse_session_remove_chan(chan);
        }
        fuse_session_destroy(session);
      }
      fuse_unmount(mountpoint, chan);
    }
    fuse_opt_free_args(&args);
  }
  return err;
}

// Overwrites the contents of `password` with zeroes, as a security measure.
// `password` may be NULL, in which case this function does nothing.
static void clear_password(char *password) {
  if (password != NULL) {
    memset(password, 0, strlen(password));
  }
}

static char *get_password_with_prompt(const char *prompt) {
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

struct mount_args {
  bool help;
  bool version;
  bool no_password;
  bool debug;
  const char *filepath;
};

struct mount_args extract_mount_arguments(int *argc, char **argv) {
  struct mount_args args = {
    .help = false,
    .version = false,
    .no_password = false,
    .debug = false,
    .filepath = NULL };
  const int n = *argc;
  int j = 1;
  assert(n >= 1);
  for (int i = j; i < n; ++i) {
    char *arg = argv[i];
    if (strcmp(arg, "-n") == 0 || strcmp(arg, "--no_password") == 0) {
      args.no_password = true;
    } else if (arg[0] != '-' && args.filepath == NULL) {
      args.filepath = arg;
    } else {
      // Keep this argument.
      argv[j++] = arg;

      // Parse options which will be passed through to fuse_main:
      //  -h / --help
      //  -V / --version
      //  -d / -odebug / -o debug
      if (strcmp(arg, "-o") == 0) {
        if (i + 1 < n) {
          arg = argv[++i];
          argv[j++] = arg;
          if (strcmp(arg, "debug") == 0) {
            args.debug = true;
          }
        }
      } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
        args.help = true;
      } else if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
        args.version = true;
      } else if (strcmp(arg, "-d") == 0 || strcmp(arg, "-odebug") == 0) {
        args.debug = true;
      }
    }
  }
  *argc = j;
  argv[j] = NULL;
  return args;
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

static bool delete_arg_if_equal(int index, const char *value, int *argc, char *argv[]) {
  if (index >= *argc || strcmp(argv[index], value) != 0) {
    return false;
  }
  CHECK(strcmp(delete_arg(index, argc, argv), value) == 0);
  return true;
}

static void print_version() {
  printf("sqlfuse version %d.%02d (database version %d)\n",
      SQLFUSE_VERSION_MAJOR, SQLFUSE_VERSION_MINOR, SQLFS_SCHEMA_VERSION);
}

static int run_help() {
  print_version();
  fputs("\nUsage:\n"
      "    sqlfuse help\n"
      "    sqlfuse create [-n|--no_password] <database>\n"
      "    sqlfuse mount [-n|--no_password] <database> <mountpoint> [FUSE options]\n"
      "    sqlfuse rekey <database>\n"
      "    sqlfuse vacuum <database>\n"
      "    sqlfuse fsck <database>\n", stdout);
  return 0;
}

static int run_create(int argc, char *argv[]) {
  bool no_password = delete_arg_if_equal(1, "-n", &argc, argv) ||
    delete_arg_if_equal(1, "--no_password", &argc, argv);
  const char *database = get_database_argument(argc, argv, false /* should_exist */);
  if (database == NULL) {
    return 1;
  }
  char *password = NULL;
  if (!no_password) {
    password = get_new_password();
    if (password == NULL) {
      return 1;
    }
  }
  bool success = sqlfs_create(database, password, getumask(), geteuid(), getegid());
  clear_password(password);
  password = NULL;
  if (!success) {
    fprintf(stderr, "Failed to create database '%s'.\n", database);
    return 1;
  }
  printf("Created database '%s' (%s)\n", database, no_password ? "not encrypted" : "encrypted");
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

  if (!validate_database_path(args.filepath, true)) {
    return 1;
  }

  char *password = NULL;
  if (!args.no_password) {
    password = get_password();
    if (password == NULL) {
      return 1;
    }
  }

  struct sqlfs *sqlfs = sqlfs_open(args.filepath, password, getumask(), geteuid(), getegid());
  clear_password(password);
  password = NULL;
  if (!sqlfs) {
    fprintf(stderr, "Failed to open database '%s'.\n", args.filepath);
    return 1;
  }
  if (sqlfs_purge_all(sqlfs) != 0) {
    fprintf(stderr, "Failed to purge unlinked entries in database '%s'.\n", args.filepath);
    return 1;
  }
  struct intmap *lookups = intmap_create();
  if (lookups == NULL) {
    fprintf(stderr, "Failed to create intmap.\n");
    sqlfs_close(sqlfs);
    return 1;
  }
  int err = sqlfuse_main(argc, argv, sqlfs, lookups);
  intmap_destroy(lookups);
  sqlfs_close(sqlfs);
  return err ? 1 : 0;
}

static int run_rekey(int argc, char *argv[]) {
  const char *database = get_database_argument(argc, argv, true /* must_exist */);
  if (database == NULL) {
    return 1;
  }

  int result = 1;  // exit failure
  struct sqlfs *sqlfs = NULL;
  char *old_password = NULL;
  char *new_password = NULL;

  old_password = get_password();
  if (old_password == NULL) {
    goto finish;
  }
  sqlfs = sqlfs_open(database, old_password, getumask(), geteuid(), getegid());
  if (sqlfs == NULL) {
    fprintf(stderr, "Failed to open database '%s'.\n", database);
    goto finish;
  }
  new_password = get_new_password();
  if (new_password == NULL) {
    goto finish;
  }
  if (!sqlfs_rekey(sqlfs, new_password)) {
    fprintf(stderr, "Could not rekey database '%s' (not writable?)\n", database);
    goto finish;
  }
  result = 0;  // exit successfully

finish:
  if (sqlfs != NULL) {
    sqlfs_close(sqlfs);
  }
  clear_password(old_password);
  clear_password(new_password);
  return result;
}

static int run_vacuum(int argc, char *argv[]) {
  const char *database = get_database_argument(argc, argv, true /* must_exist */);
  if (database == NULL) {
    return 1;
  }
  // TODO: implement this!
  fprintf(stderr, "Command 'vacuum' not yet implemented!\n");
  return 1;
}

static int run_fsck(int argc, char *argv[]) {
  const char *database = get_database_argument(argc, argv, true /* must_exist */);
  if (database == NULL) {
    return 1;
  }
  // TODO: implement this!
  fprintf(stderr, "Command 'fsck' not yet implemented!\n");
  return 1;
}

int main(int argc, char *argv[]) {
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

  if (strcmp(command, "vacuum") == 0) {
    return run_vacuum(argc, argv);
  }

  if (strcmp(command, "fsck") == 0) {
    return run_fsck(argc, argv);
  }

  if (strcmp(command, "help") == 0) {
    return run_help();
  }

  run_help();
  fprintf(stderr, "\nUnsupported command: '%s'\n", command);
  return 1;
}
