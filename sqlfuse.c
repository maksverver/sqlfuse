#include "sqlfuse.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "logging.h"
#include "sqlfs.h"

#include "fuse_lowlevel.h"

// "Blocksize for IO" as returned in the st_blksize field of struct stat.
#define BLKSIZE 4096

// Temporary! TODO: Remove this once the sqlfuse_* functions are implemented.
#pragma GCC diagnostic ignored "-Wunused-parameter"

#define TRACE(...) \
  do { \
    if (logging_enabled) { \
      fprintf(stderr, "[%s:%d] %s()", __FILE__, __LINE__, __FUNCTION__); \
      __VA_ARGS__; \
      fputc('\n', stderr); \
    } \
  } while(0)

#define TRACE_INT(i) fprintf(stderr, " %s=%lld", #i, (long long)i);
#define TRACE_UINT(ui) fprintf(stderr, " %s=%llu", #ui, (unsigned long long)ui)
#define TRACE_STR(s) trace_str(stderr, #s, s)

static void trace_str(FILE *fp, const char *key, const char *value) {
  fprintf(fp, " %s=", key);
  if (value == NULL) {
    fputs("NULL", fp);
    return;
  }
  fputc('"', fp);
  for (const char *s = value; *s; ++s) {
    if (*s >= 32 && *s < 127) {  // assumes ASCII/UTF-8, but it's 2017 ffs.
      fputc(*s, fp);
    } else {
      fprintf(fp, "\\%03o", *s);
    }
  }
  fputc('"', fp);
}

static void sqlfuse_init(void *userdata, struct fuse_conn_info *conn) {
  TRACE();
}

static void sqlfuse_destroy(void *userdata) {
  TRACE();
}

static void reply_err(fuse_req_t req, int err) {
  int res = fuse_reply_err(req, err);
  if (res != 0) {
    LOG("WARNING: fuse_reply_err(err=%d) returned %d\n", err, res);
  }
}

static void sqlfuse_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
  TRACE(TRACE_UINT(parent), TRACE_STR(name));
  LOG("sqlfuse_lookup(parent=%llu, name=%s)\n", (unsigned long long)parent, name);
  reply_err(req, ENOSYS);
}

static void sqlfuse_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup) {
  TRACE(TRACE_UINT(ino), TRACE_UINT(nlookup));
  fuse_reply_none(req);
}

static void sqlfuse_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
  TRACE(TRACE_UINT(ino));
  // Valid replies: fuse_reply_attr fuse_reply_err
  if (ino == 1) {
    // Technically, directories must have a hardlink count of 2 + number of
    // subdirectories, since each directory is referred to by its parent, the
    // '.' entry, and '..' entry of each of its subdirectories. (The root is
    // special: in that case, there is no parent directory, but instead the '..'
    // entry of the root refers to the root itself, so the count is the same).
    struct stat attr = {
      .st_ino = ino,
      .st_mode = S_IFDIR | 0755,
      .st_nlink = 2,  // directory must have two entries
      .st_uid = 0,
      .st_gid = 0,
      .st_rdev = 0,
      .st_size = 0,
      .st_blksize = BLKSIZE,  // blocksize for I/O
      .st_blocks = 0,   // number of 512B blocks allocated
      // TODO: atime, mtime, ctime? second or nanosecond resolution?
    };
    fuse_reply_attr(req, &attr, 0.0 /* attr_timeout */);
  } else {
    reply_err(req, ENOSYS);
  }
}

static void sqlfuse_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
    int to_set, struct fuse_file_info *fi) {
  TRACE(TRACE_UINT(ino), TRACE_INT(to_set));
  reply_err(req, ENOSYS);
}

static void sqlfuse_mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
    mode_t mode, dev_t rdev) {
  TRACE(TRACE_UINT(parent), TRACE_STR(name), TRACE_UINT(mode), TRACE_UINT(rdev));
  reply_err(req, ENOSYS);
}

static void sqlfuse_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode) {
  TRACE(TRACE_UINT(parent), TRACE_STR(name), TRACE_UINT(mode));
  reply_err(req, ENOSYS);
}

static void sqlfuse_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
  TRACE(TRACE_UINT(parent), TRACE_STR(name));
  reply_err(req, ENOSYS);
}

static void sqlfuse_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
  TRACE(TRACE_UINT(parent), TRACE_STR(name));
  // NOTE: deleting non-empty directories is not allowed.
  // NOTE: deleting the root directory itself is not allowed.
  reply_err(req, ENOSYS);
}

static void sqlfuse_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
    fuse_ino_t newparent, const char *newname) {
  TRACE(TRACE_UINT(parent), TRACE_STR(name), TRACE_UINT(newparent), TRACE_STR(newname));
  reply_err(req, ENOSYS);
}

static void sqlfuse_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
  TRACE(TRACE_UINT(ino));
  reply_err(req, ENOSYS);
}

static void sqlfuse_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
    struct fuse_file_info *fi) {
  TRACE(TRACE_UINT(ino), TRACE_UINT(size), TRACE_UINT(off));
  reply_err(req, ENOSYS);
}

static void sqlfuse_write(fuse_req_t req, fuse_ino_t ino, const char *buf,
    size_t size, off_t off, struct fuse_file_info *fi) {
  TRACE(TRACE_UINT(ino), TRACE_UINT(size), TRACE_UINT(off));
  reply_err(req, ENOSYS);
}

static void sqlfuse_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
  TRACE(TRACE_UINT(ino));
  reply_err(req, ENOSYS);
}

static void sqlfuse_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
  TRACE(TRACE_UINT(ino));
  reply_err(req, ENOSYS);
}

static void sqlfuse_readdir(fuse_req_t req, fuse_ino_t ino,
    size_t size, off_t off, struct fuse_file_info *fi) {
  TRACE(TRACE_UINT(ino), TRACE_UINT(size), TRACE_UINT(off));
  reply_err(req, ENOSYS);
}

static void sqlfuse_releasedir(fuse_req_t req, fuse_ino_t ino,
    struct fuse_file_info *fi) {
  TRACE(TRACE_UINT(ino));
  reply_err(req, ENOSYS);
}

const struct fuse_lowlevel_ops sqlfuse_ops = {
  .init = sqlfuse_init,
  .destroy = sqlfuse_destroy,
  .lookup = sqlfuse_lookup,
  .forget = sqlfuse_forget,
  .getattr = sqlfuse_getattr,
  .setattr = sqlfuse_setattr,
  .readlink = NULL,  // symlinks not supported. Maybe later?
  .mknod = sqlfuse_mknod,
  .mkdir = sqlfuse_mkdir,
  .unlink = sqlfuse_unlink,
  .rmdir = sqlfuse_rmdir,
  .symlink = NULL,  // symlinks not supported. Maybe later?
  .rename = sqlfuse_rename,
  .link  = NULL,  // hardlinks not supported. Maybe later?
  .open = sqlfuse_open,
  .read = sqlfuse_read,
  .write = sqlfuse_write,
  .flush = NULL,  // flush not supported
  .release = sqlfuse_release,
  .fsync = NULL,  // fsync not supported
  .opendir = sqlfuse_opendir,
  .readdir = sqlfuse_readdir,
  .releasedir = sqlfuse_releasedir,
  .fsyncdir = NULL,  // fsyncdir not supported
  .statfs = NULL,  // statfs not supported. Maybe later?
  .setxattr = NULL,  // extended attributes not supported
  .getxattr = NULL,  // extended attributes not supported
  .listxattr = NULL,  // extended attributes not supported
  .removexattr = NULL,  // extended attributes not supported
  // We don't implement custom access permission checks.
  .access = NULL,
  // We don't support atomic creation. The kernel will call mknod() followed by
  // open() instead.
  .create = NULL,
  // We don't support POSIX locking. The kernel will emulate local locking.
  .getlk = NULL,
  .setlk = NULL,
  // Not supported because we're not backed by a block device.
  .bmap = NULL,
  .ioctl = NULL,  // ioctl not supported
  .poll = NULL,  // poll not supported
  .write_buf = NULL,  // zero-copy writing not supported
  // Callback for fuse_lowlevel_notify_retrieve(), which is not used?
  .retrieve_reply = NULL,
  // Library will call forget multiple times instead.
  .forget_multi = NULL,
  // Same as POSIX lock: deferred to kernel for local implementation.
  .flock = NULL,
  .fallocate = NULL,  // fallocate not supported. Maybe later?
};
