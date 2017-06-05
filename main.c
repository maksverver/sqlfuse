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

struct args {
  bool help;
  bool version;
  bool no_password;
  bool create_db;
  const char *filepath;
};

struct args extract_arguments(int *argc, char **argv) {
  struct args args = {
    .help = false,
    .version = false,
    .no_password = false,
    .create_db = false,
    .filepath = NULL };
  const int n = *argc;
  int j = 1;
  assert(n >= 1);
  for (int i = j; i < n; ++i) {
    char *arg = argv[i];
    if (strcmp(arg, "-n") == 0 || strcmp(arg, "--no_password") == 0) {
      args.no_password = true;
    } else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--create") == 0) {
      args.create_db = true;
    } else if (arg[0] != '-' && args.filepath == NULL) {
      args.filepath = arg;
    } else {
      // Keep this argument.
      argv[j++] = arg;

      // -h/--help and -V/--version will be passed through to fuse_main().
      if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
        args.help = true;
      } else if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
        args.version = true;
      }
    }
  }
  *argc = j;
  argv[j] = NULL;
  return args;
}

// Verifies that either args->filepath exists if and only if args->create_db is false.
static bool validate_filepath(const struct args *args) {
  if (args->filepath == NULL) {
    fprintf(stderr, "Missing database path argument.\n");
    return false;
  }
  struct stat st;
  if (stat(args->filepath, &st) != 0) {
    if (errno != ENOENT) {
      perror(NULL);
      return false;
    }
    if (!args->create_db) {
      fprintf(stderr, "Database '%s' does not exist. (Use the -c/--create option to create it.)\n", args->filepath);
      return false;
    }
  } else {
    if (!S_ISREG(st.st_mode)) {
      fprintf(stderr, "Database '%s' is not a regular file.\n", args->filepath);
      return false;
    }
    if (args->create_db) {
      fprintf(stderr, "Database '%s' already exists. (Remove the -c/--create option to open it.)\n", args->filepath);
      return false;
    }
  }
  return true;
}

int main(int argc, char *argv[]) {
  struct args args = extract_arguments(&argc, argv);

  if (args.help || args.version) {
    if (args.version) {
      // TODO: print sqlfuse version?
      printf("sqlfuse version 0.0\n");
    }
    if (args.help) {
      fputs(
          "Usage: sqlfuse [options] <database> <mountpoint>\n"
          "\n"
          "Supported sqlfuse options:\n"
          "    -n   --no_password    don't prompt for password (disables encryption)\n"
          "    -c   --create         create a new database\n"
          "\n",
          stdout);
    }

    // BUG: this prints usage for the high-level FUSE API, while we're really
    // using the low-level API. TODO: fix this somehow?
    fuse_main(argc, argv, (const struct fuse_operations*)NULL, NULL);
    return 0;
  }

  if (!validate_filepath(&args)) {
    return 1;
  }

  char *password = NULL;
  if (!args.no_password) {
    password = getpass("Password: ");
    if (password == NULL) {
      fprintf(stderr, "Failed to read password.\n");
      return 1;
    }
    if (!*password) {
      fprintf(stderr, "Empty password not accepted. (Use the -n/--no_password option to disable encryption.)\n");
      return 1;
    }
  }

  struct sqlfs *sqlfs = sqlfs_create(args.filepath, password, getumask(), geteuid(), getegid());
  if (password != NULL) {
    // Clean password for security. The derived key is still in memory, but it's
    // better than nothing.
    memset(password, 0, strlen(password));
    password = NULL;
  }
  if (!sqlfs) {
    fprintf(stderr, "Failed to open database '%s'.\n", args.filepath);
    return 1;
  }
  struct intmap *lookups = intmap_create();
  if (lookups == NULL) {
    fprintf(stderr, "Failed to create intmap.\n");
    sqlfs_destroy(sqlfs);
    return 1;
  }
  int err = sqlfuse_main(argc, argv, sqlfs, lookups);
  intmap_destroy(lookups);
  sqlfs_destroy(sqlfs);
  return err ? 1 : 0;
}
