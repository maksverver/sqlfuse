#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "fuse_lowlevel.h"

#include "logging.h"
#include "sqlfs.h"
#include "sqlfuse.h"

// Runs the FUSE low-level main loop. Returns 0 on success.
// Based on: https://github.com/libfuse/libfuse/blob/fuse_2_6_bugfix/example/hello_ll.c
static int sqlfuse_main(int argc, char* argv[]) {
  int err = -1;
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  char *mountpoint = NULL;
  int multithreaded = 0;
  int foreground = 0;

  if (fuse_parse_cmdline(&args, &mountpoint, &multithreaded, &foreground) == 0) {
    if (multithreaded) {
      fprintf(stderr, "WARNING! Multi-threading not supported; running single-threaded. (Pass -s to suppress this warning.)\n");
    }
    struct fuse_chan *chan = fuse_mount(mountpoint, &args);
    if (chan != NULL) {
      struct fuse_session *session = fuse_lowlevel_new(
          &args, &sqlfuse_ops, sizeof(sqlfuse_ops), NULL /* userdata */);
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

char *extract_first_argument(int *argc, char ***argv) {
  if (*argc < 2) {
    return NULL;
  }
  char *result = (*argv)[1];
  int n = --*argc;
  for (int i = 1; i < n; ++i) {
    (*argv)[i] = (*argv)[i + 1];
  }
  (*argv)[n] = NULL;
  return result;
}

// Usage:
//
//  sqlfuse <database path> <mountpoint> [fuse options]
//
// Will prompt for the password before mounting.
//
// Note: access checking is not implemented. Add -o default_permissions to the
// command line to defer access checking to the kernel, especially when using
// -o allow_others (which is not recommended!)
int main(int argc, char *argv[]) {
  char *filepath = extract_first_argument(&argc, &argv);
  if (filepath == NULL) {
    fprintf(stderr, "Missing database path argument.\n");
    return 1;
  }
  char *password = getpass("Password: ");
  if (password == NULL) {
    fprintf(stderr, "Failed to read password.\n");
    return 1;
  }
  if (!*password) {
    fprintf(stderr, "WARNING! Empty password given. No encryption will be used!\n");
    password = NULL;
  }
  struct sqlfs *sqlfs = sqlfs_create(filepath, password);
  if (password != NULL) {
    /* Clean password for security reasons. */
    memset(password, 0, strlen(password));
    password = NULL;
  }
  if (!sqlfs) {
    fprintf(stderr, "Failed to open database %s!\n", filepath);
    fclose(stderr);
    return 1;
  }
  int err = sqlfuse_main(argc, argv);
  sqlfs_destroy(sqlfs);
  return err ? 1 : 0;
}
