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

// We don't use generation numbers! But we also don't guarantee not to re-use
// any inode numbers. That means the file system cannot safely be used over NFS!
#define GENERATION 1

// Temporary! TODO: Remove this once the sqlfuse_* functions are implemented.
#pragma GCC diagnostic ignored "-Wunused-parameter"

// Describes an open directory handle.
//
// In the beginning (i.e. immediately after a call to sqlfuse_opendir()):
//
//  next_name == NULL && at_end == false.
//
// If sqlfuse_readdir() fills its buffer but there are more entries remaining,
// it will set next_name to indicate where the next call should continue:
//
//  next_name != NULL && at_end == false.
//
// If on the other hand, the last entry in the directory has been placed in a
// buffer, it will set:
//
//  next_name == NULL && at_end == true.
//
// If at_end == true(), sqlfuse_readdir() will return an empty buffer to
// indicate to FUSE that the end of the directory has been reached.
//
// Note that the dir_handle structure is allocated by sqlfuse_opendir() and
// freed by sqlfuse_closedir(). next_name is allocated by sqlfuse_readdir(),
// and freed by sqlfuse_readdir() or sqlfuse_closedir().
struct dir_handle {
  char *next_name;
  bool at_end;
};

// Maximum buffer size for reading directory entries. Should be large enough
// so that any entry will fit. (We don't enforce a limit on file names, but
// presumably the kernel does, and it will be much less than 256 KiB.)
#define MAX_BUF_SIZE (256 * 1024) /* 256 KiB */

#define TRACE(...) \
  do { \
    if (logging_enabled) { \
      fprintf(stderr, "[%s:%d] %s()", __FILE__, __LINE__, __func__); \
      __VA_ARGS__; \
      fputc('\n', stderr); \
    } \
  } while(0)

#define TRACE_INT(i) fprintf(stderr, " %s=%lld", #i, (long long)i);
#define TRACE_UINT(ui) fprintf(stderr, " %s=%llu", #ui, (unsigned long long)ui)
#define TRACE_STR(s) trace_str(stderr, #s, s)
#define TRACE_MODE(m) fprintf(stderr, " %s=0%o", #m, (int)m)

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

static void reply_entry(fuse_req_t req, const struct fuse_entry_param *entry) {
  int res = fuse_reply_entry(req, entry);
  if (res != 0) {
    LOG("WARNING: fuse_reply_entry() returned %d\n", res);
  } else {
    // TODO: accounting of lookup counts!
  }
}

static void reply_buf(fuse_req_t req, const char *buf, size_t size) {
  int res = fuse_reply_buf(req, buf, size);
  if (res != 0) {
    LOG("WARNING: fuse_reply_buf() returned %d\n", res);
  }
}

static void sqlfuse_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
  TRACE(TRACE_UINT(parent), TRACE_STR(name));

  struct fuse_entry_param entry;
  memset(&entry, 0, sizeof(entry));
  int err = sqlfs_stat_entry(fuse_req_userdata(req), parent, name, &entry.attr);
  if (err != 0) {
    return reply_err(req, err);
  }
  entry.ino = entry.attr.st_ino;
  entry.generation = GENERATION;
  return reply_entry(req, &entry);
}

static void sqlfuse_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup) {
  TRACE(TRACE_UINT(ino), TRACE_UINT(nlookup));
  fuse_reply_none(req);
}

static void sqlfuse_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
  TRACE(TRACE_UINT(ino));

  struct stat attr;
  int err = sqlfs_stat(fuse_req_userdata(req), ino, &attr);
  if (err != 0) {
    return reply_err(req, err);
  }
  fuse_reply_attr(req, &attr, 0.0 /* attr_timeout */);
}

static void sqlfuse_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
    int to_set, struct fuse_file_info *fi) {
  TRACE(TRACE_UINT(ino), TRACE_INT(to_set));
  reply_err(req, ENOSYS);
}

static void sqlfuse_mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
    mode_t mode, dev_t rdev) {
  TRACE(TRACE_UINT(parent), TRACE_STR(name), TRACE_MODE(mode), TRACE_UINT(rdev));
  struct stat attr;
  int err = sqlfs_mknod(fuse_req_userdata(req), parent, name, mode, &attr);
  if (err == 0) {
    fuse_reply_attr(req, &attr, 0.0 /* attr_timeout */);
  } else {
    reply_err(req, err);
  }
}

static void sqlfuse_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode) {
  TRACE(TRACE_UINT(parent), TRACE_STR(name), TRACE_MODE(mode));

  struct fuse_entry_param entry;
  memset(&entry, 0, sizeof(entry));
  int err = sqlfs_mkdir(fuse_req_userdata(req), parent, name, mode, &entry.attr);
  if (err != 0) {
    return reply_err(req, err);
  }
  entry.ino = entry.attr.st_ino;
  entry.generation = GENERATION;
  return reply_entry(req, &entry);
}

static void sqlfuse_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
  TRACE(TRACE_UINT(parent), TRACE_STR(name));
  struct sqlfs *const sqlfs = fuse_req_userdata(req);
  ino_t child_ino = 0;
  int err = sqlfs_unlink(sqlfs, parent, name, &child_ino);
  if (err == 0) {
    // TODO: only do this if lookup count is zero!
    err = sqlfs_purge(sqlfs, child_ino);
  }
  reply_err(req, err);
}

static void sqlfuse_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
  TRACE(TRACE_UINT(parent), TRACE_STR(name));
  struct sqlfs *const sqlfs = fuse_req_userdata(req);
  ino_t child_ino = 0;
  int err = sqlfs_rmdir(sqlfs, parent, name, &child_ino);
  if (err == 0) {
    // TODO: only do this if lookup count is zero!
    err = sqlfs_purge(sqlfs, child_ino);
  }
  return reply_err(req, err);
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
  struct dir_handle *dir_handle = calloc(1, sizeof(struct dir_handle));
  fi->fh = (uint64_t) dir_handle;
  int res = fuse_reply_open(req, fi);
  if (res != 0) {
    LOG("WARNING: fuse_reply_open() returned %d\n", res);
  }
}

static void sqlfuse_readdir(fuse_req_t req, fuse_ino_t ino,
    size_t size, off_t off, struct fuse_file_info *fi) {
  TRACE(TRACE_UINT(ino), TRACE_UINT(size), TRACE_UINT(off));

  struct dir_handle *dir_handle = (struct dir_handle *)fi->fh;
  if (dir_handle->at_end) {
    // At end: send empty buffer.
    reply_buf(req, NULL, 0);
    return;
  }

  if (size > MAX_BUF_SIZE) {
    size = MAX_BUF_SIZE;
  }
  char *buf = malloc(size);
  CHECK(buf);
  size_t pos = 0;

  bool at_beginning = dir_handle->next_name == NULL;

  if (at_beginning) {
    // First time we're called. Add a "." entry for the directory itself.
    const struct stat st = {
      .st_ino = ino,
      .st_mode = S_IFDIR };
    const off_t off = 0;  // Offset of next entry. Not used.
    pos = fuse_add_direntry(req, buf, size, ".", &st, off);
    // This entry should always fit, unless `size` was unreasonably small.
    CHECK(pos <= size);
  }

  struct sqlfs *sqlfs = fuse_req_userdata(req);
  sqlfs_dir_open(sqlfs, ino, dir_handle->next_name);
  free(dir_handle->next_name);
  dir_handle->next_name = NULL;
  for (;;) {
    struct stat st = {0};
    const char *name = NULL;
    if (!sqlfs_dir_next(sqlfs, &name, &st.st_ino, &st.st_mode)) {
      // Reached end of directory listing.
      dir_handle->at_end = true;
      break;
    }
    at_beginning = false;
    off_t off = 0;  // Offset of next entry. Not used.
    size_t len = fuse_add_direntry(req, buf + pos, size - pos, name, &st, off);
    if (len > size - pos) {
      // Buffer full. Save next filename for continuing later.
      char *name_copy = strdup(name);
      CHECK(name_copy);
      dir_handle->next_name = name_copy;
      break;
    }
    pos += len;
  }
  sqlfs_dir_close(sqlfs);

  if (at_beginning) {
    // If the directory didn't contain ANY entries (not even the .. entry)
    // then it wasn't a directory. (It may not even be a file, but we cannot
    // distinguish between ENOENT and ENOTDIR here, so we just return ENOTDIR).
    reply_err(req, ENOTDIR);
  } else {
    // The only reason why we wouldn't have filled in any entries here, is if
    // the buffer size was too small to fit the next entry. That shouldn't be
    // possible (we assume the file name length is limited by the kernel, and
    // that FUSE will pass a sufficiently large `size` so that we can always
    // make some progress).
    CHECK(pos > 0);
    reply_buf(req, buf, pos);
  }

  free(buf);
}

static void sqlfuse_releasedir(fuse_req_t req, fuse_ino_t ino,
    struct fuse_file_info *fi) {
  TRACE(TRACE_UINT(ino));
  struct dir_handle *dir_handle = (struct dir_handle *)fi->fh;
  free(dir_handle->next_name);
  free(dir_handle);
  reply_err(req, 0);
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
