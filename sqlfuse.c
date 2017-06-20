#include "sqlfuse.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "intmap.h"
#include "logging.h"
#include "sqlfs.h"

#include "fuse_lowlevel.h"

bool sqlfuse_tracing_enabled = false;

// We don't use generation numbers! But we also don't guarantee not to re-use
// any inode numbers. That means the file system cannot safely be used over NFS!
#define GENERATION 1

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
// freed by sqlfuse_releasedir(). next_name is allocated by sqlfuse_readdir(),
// and freed by sqlfuse_readdir() or sqlfuse_closedir().
struct dir_handle {
  char *next_name;
  bool at_end;
};

// Maximum buffer size for reading directory entries. Should be large enough
// so that any entry will fit. (We don't enforce a limit on file names, but
// presumably the kernel does, and it will be much less than 256 KiB.)
#define MAX_BUF_SIZE (256 * 1024) /* 256 KiB */

#define TRACE_BEGIN(...) \
  do { \
    if (sqlfuse_tracing_enabled) { \
      fprintf(stderr, "[%s:%d] %s() ->", __FILE__, __LINE__, __func__); \
      __VA_ARGS__; \
      fputc('\n', stderr); \
    } \
  } while(0)

#define TRACE_END() \
  do { \
    if (sqlfuse_tracing_enabled) { \
      fprintf(stderr, "[%s:%d] %s() <-\n", __FILE__, __LINE__, __func__); \
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

static struct sqlfuse_userdata* req_userdata(fuse_req_t req) {
  return fuse_req_userdata(req);
}

static struct sqlfs *req_sqlfs(fuse_req_t req) {
  return req_userdata(req)->sqlfs;
}

static struct intmap *req_lookups(fuse_req_t req) {
  return req_userdata(req)->lookups;
}

// Lookup accounting: decrease an inode's lookup count, and purge it if it
// becomes zero.
//
// Returns the result of sqlfs_purge() if the inode was purged, or 0 otherwise.
static int forget(struct sqlfuse_userdata *userdata, ino_t ino, int64_t nlookup) {
  CHECK(nlookup >= 0);
  int64_t new_count = intmap_update(userdata->lookups, ino, -nlookup);
  if (new_count < 0) {
    // This should never happen, unless there is some inconsistency between how
    // FUSE and I account for lookups!
    LOG("WARNING: negative lookup count %lld for inode number %lld!\n", (long long)new_count, (long long)ino);
  }
  if (new_count > 0) {
    return 0;
  }
  return sqlfs_purge(userdata->sqlfs, ino);
}

static int maybe_purge(struct sqlfuse_userdata *userdata, ino_t ino) {
  return forget(userdata, ino, 0);
}

static void sqlfuse_init(void *userdata, struct fuse_conn_info *conn) {
  (void)conn;  // Unused.

  TRACE_BEGIN();
  struct sqlfuse_userdata *sqlfuse_userdata = userdata;
  intmap_update(sqlfuse_userdata->lookups, SQLFS_INO_ROOT, 1);
  TRACE_END();
}

static void sqlfuse_destroy(void *userdata) {
  TRACE_BEGIN();

  // Purge any remaining inodes with nonzero lookup count.
  struct sqlfuse_userdata *sqlfuse_userdata = userdata;
  struct intmap *lookups = sqlfuse_userdata->lookups;
  int64_t key, value;
  while (intmap_retrieve_one(lookups, &key, &value)) {
    if (sqlfuse_tracing_enabled) {
      fprintf(stderr, "Purging ino=%lld nlookup=%lld\n", (long long)key, (long long)value);
    }
    forget(sqlfuse_userdata, (ino_t)key, value);
  }
  TRACE_END();
}

#define REPLY_NONE(req) reply_none(__FILE__, __LINE__, __func__, req)
#define REPLY_ERR(req, err) reply_err(__FILE__, __LINE__, __func__, req, err)
#define REPLY_ENTRY(req, entry) reply_entry(__FILE__, __LINE__, __func__, req, entry)
#define REPLY_BUF(req, buf, size) reply_buf(__FILE__, __LINE__, __func__, req, buf, size)
#define REPLY_ATTR(req, attr) reply_attr(__FILE__, __LINE__, __func__, req, attr)
#define REPLY_OPEN(req, fi) reply_open(__FILE__, __LINE__, __func__, req, fi)
#define REPLY_WRITE(req, count) reply_write(__FILE__, __LINE__, __func__, req, count)

static void reply_none(const char *file, int line, const char *func, fuse_req_t req) {
  if (sqlfuse_tracing_enabled) {
    fprintf(stderr, "[%s:%d] %s() <- none\n", file, line, func);
  }
  fuse_reply_none(req);
}

static void reply_err(const char *file, int line, const char *func, fuse_req_t req, int err) {
  if (sqlfuse_tracing_enabled) {
    fprintf(stderr, "[%s:%d] %s() <- err=%d\n", file, line, func, err);
  }
  int res = fuse_reply_err(req, err);
  if (res != 0) {
    LOG("WARNING: fuse_reply_err(err=%d) returned %d\n", err, res);
  }
}

static void reply_entry(const char *file, int line, const char *func, fuse_req_t req, const struct fuse_entry_param *entry) {
  if (sqlfuse_tracing_enabled) {
    fprintf(stderr, "[%s:%d] %s() <- entry{ino=%lld}\n", file, line, func, (long long)entry->ino);
  }
  // Must grab lookups pointer first, because `req` is invalid after calling fuse_reply_entry!
  struct intmap *const lookups = req_lookups(req);
  int res = fuse_reply_entry(req, entry);
  if (res != 0) {
    LOG("WARNING: fuse_reply_entry() returned %d\n", res);
  } else {
    // Lookup accounting: increase lookup count.
    intmap_update(lookups, entry->ino, 1);
  }
}

static void reply_buf(const char *file, int line, const char *func, fuse_req_t req, const char *buf, size_t size) {
  if (sqlfuse_tracing_enabled) {
    fprintf(stderr, "[%s:%d] %s() <- buf=%p size=%lld\n", file, line, func, buf, (long long)size);
  }
  int res = fuse_reply_buf(req, buf, size);
  if (res != 0) {
    LOG("WARNING: fuse_reply_buf() returned %d\n", res);
  }
}

static void reply_attr(const char *file, int line, const char *func, fuse_req_t req, const struct stat *attr) {
  if (sqlfuse_tracing_enabled) {
    // We only print the most interesting fields of attr for now.
    fprintf(stderr, "[%s:%d] %s() <- attr{ino=%lld mode=0%o nlink=%d size=%lld}\n",
        file, line, func, (long long)attr->st_ino, (int)attr->st_mode, (int)attr->st_nlink, (long long)attr->st_size);
  }
  int res = fuse_reply_attr(req, attr, 0.0 /* attr_timeout */);
  if (res != 0) {
    LOG("WARNING: fuse_reply_attr() returned %d\n", res);
  }
}

static void reply_open(const char *file, int line, const char *func, fuse_req_t req, struct fuse_file_info *fi) {
  if (sqlfuse_tracing_enabled) {
    fprintf(stderr, "[%s:%d] %s() <- open fh=0x%llx direct_io=%d keep_cache=%d\n",
        file, line, func, (long long)fi->fh, (int)fi->direct_io, (int)fi->keep_cache);
  }
  int res = fuse_reply_open(req, fi);
  if (res != 0) {
    LOG("WARNING: fuse_reply_open() returned %d\n", res);
  }
}

static void reply_write(const char *file, int line, const char *func, fuse_req_t req, size_t count) {
  if (sqlfuse_tracing_enabled) {
    fprintf(stderr, "[%s:%d] %s() <- write count=%lld\n", file, line, func, (long long)count);
  }
  int res = fuse_reply_write(req, count);
  if (res != 0) {
    LOG("WARNING: fuse_reply_write() returned %d\n", res);
  }
}

static void sqlfuse_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
  TRACE_BEGIN(TRACE_UINT(parent), TRACE_STR(name));

  struct fuse_entry_param entry;
  memset(&entry, 0, sizeof(entry));
  int err = sqlfs_stat_entry(req_sqlfs(req), parent, name, &entry.attr);
  if (err != 0) {
    REPLY_ERR(req, err);
    return;
  }
  entry.ino = entry.attr.st_ino;
  entry.generation = GENERATION;
  REPLY_ENTRY(req, &entry);
}

static void sqlfuse_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup) {
  TRACE_BEGIN(TRACE_UINT(ino), TRACE_UINT(nlookup));
  CHECK(nlookup < INT64_MAX);
  int err = forget(req_userdata(req), ino, (int64_t)nlookup);
  if (err != 0) {
    // This is a pretty serious condition: we failed to purge a deleted file or
    // directory, but we can't report the error upstream.
    LOG("ERROR: failed to purge ino=%lld (err=%d)!\n", (long long)ino, err);
  }
  REPLY_NONE(req);
}

static void sqlfuse_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
  TRACE_BEGIN(TRACE_UINT(ino));
  (void)fi;  // Unused. (Reserved by FUSE for future use. Should be NULL, now.)
  struct stat attr;
  int err = sqlfs_stat(req_sqlfs(req), ino, &attr);
  if (err != 0) {
    REPLY_ERR(req, err);
    return;
  }
  REPLY_ATTR(req, &attr);
}

static void sqlfuse_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
    int to_set, struct fuse_file_info *fi) {
  TRACE_BEGIN(TRACE_UINT(ino), TRACE_INT(to_set));

  // Under very specific conditions, fi->fh would contain an open file handle
  // (see fuse_lowlevel.h for details). We assume it's NULL, and don't use it.
  (void)fi;

  unsigned sqlfs_to_set = 0;
  if (to_set & FUSE_SET_ATTR_MODE) {
    sqlfs_to_set |= SQLFS_SET_ATTR_MODE;
  }
  if (to_set & FUSE_SET_ATTR_UID) {
    sqlfs_to_set |= SQLFS_SET_ATTR_UID;
  }
  if (to_set & FUSE_SET_ATTR_GID) {
    sqlfs_to_set |= SQLFS_SET_ATTR_GID;
  }
  if (to_set & FUSE_SET_ATTR_SIZE) {
    sqlfs_to_set |= SQLFS_SET_ATTR_SIZE;
  }
  if (to_set & FUSE_SET_ATTR_MTIME) {
    sqlfs_to_set |= SQLFS_SET_ATTR_MTIME;
  }
  // Note: FUSE_SET_ATTR_ATIME will be ignored.

  struct stat new_attr;
  int err = sqlfs_set_attr(req_sqlfs(req), ino, attr, sqlfs_to_set, &new_attr);
  if (err == 0) {
    REPLY_ATTR(req, &new_attr);
  } else {
    REPLY_ERR(req, err);
  }
}

static void sqlfuse_mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
    mode_t mode, dev_t rdev) {
  TRACE_BEGIN(TRACE_UINT(parent), TRACE_STR(name), TRACE_MODE(mode), TRACE_UINT(rdev));
  struct fuse_entry_param entry;
  memset(&entry, 0, sizeof(entry));
  int err = sqlfs_mknod(req_sqlfs(req), parent, name, mode, &entry.attr);
  if (err != 0) {
    REPLY_ERR(req, err);
    return;
  }
  entry.ino = entry.attr.st_ino;
  entry.generation = GENERATION;
  REPLY_ENTRY(req, &entry);
}

static void sqlfuse_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode) {
  TRACE_BEGIN(TRACE_UINT(parent), TRACE_STR(name), TRACE_MODE(mode));

  struct fuse_entry_param entry;
  memset(&entry, 0, sizeof(entry));
  int err = sqlfs_mkdir(req_sqlfs(req), parent, name, mode, &entry.attr);
  if (err != 0) {
    REPLY_ERR(req, err);
    return;
  }
  entry.ino = entry.attr.st_ino;
  entry.generation = GENERATION;
  REPLY_ENTRY(req, &entry);
}

static void sqlfuse_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
  TRACE_BEGIN(TRACE_UINT(parent), TRACE_STR(name));
  struct sqlfuse_userdata *const userdata = req_userdata(req);
  ino_t child_ino = SQLFS_INO_NONE;
  int err = sqlfs_unlink(userdata->sqlfs, parent, name, &child_ino);
  if (err == 0) {
    err = maybe_purge(userdata, child_ino);
  }
  REPLY_ERR(req, err);
}

static void sqlfuse_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
  TRACE_BEGIN(TRACE_UINT(parent), TRACE_STR(name));
  struct sqlfuse_userdata *const userdata = req_userdata(req);
  ino_t child_ino = SQLFS_INO_NONE;
  int err = sqlfs_rmdir(userdata->sqlfs, parent, name, &child_ino);
  if (err == 0) {
    err = maybe_purge(userdata, child_ino);
  }
  REPLY_ERR(req, err);
}

static void sqlfuse_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
    fuse_ino_t newparent, const char *newname) {
  TRACE_BEGIN(TRACE_UINT(parent), TRACE_STR(name), TRACE_UINT(newparent), TRACE_STR(newname));
  struct sqlfuse_userdata *const userdata = req_userdata(req);
  ino_t unlinked_ino = SQLFS_INO_NONE;
  int err = sqlfs_rename(userdata->sqlfs, parent, name, newparent, newname, &unlinked_ino);
  if (err == 0 && unlinked_ino != SQLFS_INO_NONE) {
    err = maybe_purge(userdata, unlinked_ino);
  }
  REPLY_ERR(req, err);
}

static void sqlfuse_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
    struct fuse_file_info *fi) {
  TRACE_BEGIN(TRACE_UINT(ino), TRACE_UINT(size), TRACE_UINT(off));
  (void)fi;  // Unused.
  char *buf = malloc(size);
  if (buf == NULL) {
    LOG("WARNING: unable to allocate %lld bytes!\n", (long long)size);
    // Return EIO because there doesn't seem to be a more suitable errno.
    REPLY_ERR(req, EIO);
    return;
  }
  size_t size_read = 0;
  const int err = sqlfs_read(req_sqlfs(req), ino, off, size, buf, &size_read);
  if (err != 0) {
    REPLY_ERR(req, err);
  } else {
    REPLY_BUF(req, buf, size_read);
  }
  free(buf);
}

static void sqlfuse_write(fuse_req_t req, fuse_ino_t ino, const char *buf,
    size_t size, off_t off, struct fuse_file_info *fi) {
  TRACE_BEGIN(TRACE_UINT(ino), TRACE_UINT(size), TRACE_UINT(off));
  (void)fi;  // Unused
  const int err = sqlfs_write(req_sqlfs(req), ino, off, size, buf);
  if (err != 0) {
    REPLY_ERR(req, err);
  } else {
    REPLY_WRITE(req, size);
  }
}

static void sqlfuse_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
  TRACE_BEGIN(TRACE_UINT(ino));
  struct dir_handle *dir_handle = calloc(1, sizeof(struct dir_handle));
  fi->fh = (uint64_t) dir_handle;
  REPLY_OPEN(req, fi);
}

static void sqlfuse_readdir(fuse_req_t req, fuse_ino_t ino,
    size_t size, off_t off, struct fuse_file_info *fi) {
  TRACE_BEGIN(TRACE_UINT(ino), TRACE_UINT(size), TRACE_UINT(off));

  struct dir_handle *dir_handle = (struct dir_handle *)fi->fh;
  if (dir_handle->at_end) {
    // At end: send empty buffer.
    REPLY_BUF(req, NULL, 0);
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

  struct sqlfs *const sqlfs = req_sqlfs(req);
  sqlfs_dir_open(sqlfs, ino, dir_handle->next_name);
  free(dir_handle->next_name);
  dir_handle->next_name = NULL;
  for (;;) {
    struct stat st = {0};
    const char *name = NULL;
    // We currently ignore errors, treating them as end-of-directory.
    if (sqlfs_dir_next(sqlfs, &name, &st.st_ino, &st.st_mode) != 0 || name == NULL) {
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
    REPLY_ERR(req, ENOTDIR);
  } else {
    // The only reason why we wouldn't have filled in any entries here, is if
    // the buffer size was too small to fit the next entry. That shouldn't be
    // possible (we assume the file name length is limited by the kernel, and
    // that FUSE will pass a sufficiently large `size` so that we can always
    // make some progress).
    CHECK(pos > 0);
    REPLY_BUF(req, buf, pos);
  }

  free(buf);
}

static void sqlfuse_releasedir(fuse_req_t req, fuse_ino_t ino,
    struct fuse_file_info *fi) {
  TRACE_BEGIN(TRACE_UINT(ino));
  struct dir_handle *dir_handle = (struct dir_handle *)fi->fh;
  free(dir_handle->next_name);
  free(dir_handle);
  REPLY_ERR(req, 0);
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
  .open = NULL,  // we implement stateless file I/O
  .read = sqlfuse_read,
  .write = sqlfuse_write,
  .flush = NULL,  // flush not supported
  .release = NULL,  // we implement stateless file I/O
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
};
