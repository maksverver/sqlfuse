#include "sqlfuse.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "logging.h"
#include "sqlfs.h"

#include "fuse_lowlevel.h"

static void sqlfuse_init(void *userdata, struct fuse_conn_info *conn) {
}

static void sqlfuse_destroy(void *userdata) {
}

static void sqlfuse_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
}

static void sqlfuse_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup) {
}

static void sqlfuse_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
}

static void sqlfuse_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
    int to_set, struct fuse_file_info *fi) {
}

static void sqlfuse_mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
    mode_t mode, dev_t rdev) {
}

static void sqlfuse_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name,
    mode_t mode) {
}

static void sqlfuse_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
}

static void sqlfuse_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
}

static void sqlfuse_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
    fuse_ino_t newparent, const char *newname) {
}

static void sqlfuse_open(fuse_req_t req, fuse_ino_t ino,
    struct fuse_file_info *fi) {
}

static void sqlfuse_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
    struct fuse_file_info *fi) {
}

static void sqlfuse_write(fuse_req_t req, fuse_ino_t ino, const char *buf,
    size_t size, off_t off, struct fuse_file_info *fi) {
}

static void sqlfuse_release(fuse_req_t req, fuse_ino_t ino,
    struct fuse_file_info *fi) {
}

static void sqlfuse_opendir(fuse_req_t req, fuse_ino_t ino,
    struct fuse_file_info *fi) {
}

static void sqlfuse_readdir(fuse_req_t req, fuse_ino_t ino,
    size_t size, off_t off, struct fuse_file_info *fi) {
}

static void sqlfuse_releasedir(fuse_req_t req, fuse_ino_t ino,
    struct fuse_file_info *fi) {
}

static void sqlfuse_forget_multi(fuse_req_t req, size_t count,
    struct fuse_forget_data *forgets) {
  for (size_t i = 0; i < count; ++i) {
    sqlfuse_forget(req, forgets[i].ino, forgets[i].nlookup);
  }
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
  .forget_multi = sqlfuse_forget_multi,
  // Same as POSIX lock: deferred to kernel for local implementation.
  .flock = NULL,
  .fallocate = NULL,  // fallocate not supported. Maybe later?
};
