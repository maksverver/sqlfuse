// SQLFS is a filesystem stored in a SQLite3 database, optionally encrypted
// by a password, using the SQLCipher extension.
//
// All functions declared in this file are thread-compatible, NOT thread-safe!

#ifndef SQLFS_H_INCLUDED
#define SQLFS_H_INCLUDED

#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// The state for a single filesystem.
struct sqlfs;

// Opens an existing or creates a new filesystem at the given path. Whether a
// new filesystem is created or not depends on whether the database already
// exists or not.
//
// Returns NULL if the file could not be opened (e.g. invalid path, incorrect
// password, invalid file format, etc.). Otherwise, returns a pointer to the
// filesystem state, which must be destroyed by sqlfs_destroy() later.
//
//  filepath: path to the database file.
//  password: password to use. May be NULL to disable encryption.
//  umask: umask to use for this session
//  uid: user id to use for this session
//  gid: group id to use for this session
struct sqlfs *sqlfs_create(
    const char *filepath, const char *password,
    mode_t umask, uid_t uid, gid_t gid);

// Destroys the filesystem state. Afterwards, the state should not be used.
void sqlfs_destroy(struct sqlfs *sqlfs);

// Retrieves metadata for the given inode number.
//
// Returns:
//  0 on success
//  ENOENT if the file is not found
//  EIO if a database operation failed
int sqlfs_stat(struct sqlfs *sqlfs, ino_t ino, struct stat *stat);

// Retrieves metadata for a file identified by its name in a directory.
//
// Returns:
//  0 on success
//  EINVAL if the file name is invalid
//  ENOENT if the file is not found
//  EOI if the database operation failed
int sqlfs_stat_entry(struct sqlfs *sqlfs, ino_t dir_ino, const char *name,
    struct stat *stat);

// Creates a subdirectory. On success, the metadata of the newly-created
// directory is returned in *stat. Mode will be masked by the session's umask.
//
// Returns:
//  0 on success
//  EINVAL if the given name is invalid
//  ENOENT if the parent directory does not exist
//  EEXIST if a file/directory with the given name already exists
//  EIO if a database operation failed
int sqlfs_mkdir(struct sqlfs *sqlfs, ino_t dir_ino, const char *name, mode_t mode,
    struct stat *stat);

// Removes a subdirectory. On success, the inode number of the removed directory
// is written to *child_ino. The directory is not actually deleted until
// sqlfs_purge() is called.
//
// Returns:
//  0 on success
//  EINVAL if the name is invalid ("." or empty)
//  ENOENT if the directory to be removed is not found
//  ENOTDIR if the named entry does not refer to a directory
//  EBUSY if the named entry refers to the root directory
//  ENOTEMPTY if the named directory is not empty
//  EIO if a database operation fails
int sqlfs_rmdir(struct sqlfs *sqlfs, ino_t dir_ino, const char *name, ino_t *child_ino);

// The sqlfs_dir_open(), sqlfs_dir_next() and sqlfs_dir_close() functions are
// used to read the contents of a directory. The first entry is always ".." and
// refers to the directory's parent directory. The following entries are given
// in lexicographical order. Note that the "." entry is not included at all!
//
// Within a single sqlfs instance, only one directory may be opened at a time,
// and no other operations (including sqlfs_destroy()) may be performed until
// it is closed. That means the user should typically keep a directory open
// for a very short time only, and treat the open, next, and close calls as a
// single operation.
//
// To enumerate the contents of large directories in smaller chunks, use the
// `start_name` argument to sqlfs_dir_open() to resume.
//
// Example usage:
//
//   struct sqlfs *sqlfs = sqlfs_create(..);
//   struct ino_t dir_ino = 1;
//   struct sqlfs_dir *dir = sqlfs_dir_open(sqlfs, dir_ino, NULL);
//
//   const char *name;
//   ino_t ino;
//   mode_t mode;
//   while (sqlfs_dir_next(dir, &name, &ino, &mode)) {
//     printf("name=%s ino=%d mode=0%o\n", name, ino, mode);
//   }
//
//   sqlfs_dir_close(dir);
//

// Opens a directory for reading. `ino` must refer to a directory. `start_name`
// can be NULL (or equivalently, "" or "..") to start at the beginning, or the
// name of the next entry. The value of `start_name` is typically the last name
// returned by sqlfs_dir_next() before closing the directory. (It's possible
// that sqlfs_dir_next() returns a different entry, if the named entry has been
// deleted since.)
void sqlfs_dir_open(struct sqlfs *sqlfs, ino_t ino, const char *start_name);

// Reads the next directory entry, storing its name into *name, its inode
// number into *ino, and (only!) the file-type bits of its mode into *mode,
// and returns true.
//
// If there are no more entries, this function returns false instead, and the
// contents of the output arguments is undefined.
//
// The buffer pointed to by *name is valid only until the next call to
// sqlfs_dir_next() or sqlfs_dir_close().
bool sqlfs_dir_next(struct sqlfs *sqlfs, const char **name, ino_t *ino, mode_t *mode);

// Closes a directory opened previously by sqlfs_dir_open().
void sqlfs_dir_close(struct sqlfs *sqlfs);

// Creates a new file in the given directory.
//
// `dir_ino` refers to the existing directory in which the file will be created.
// `name` contains the filename, which must be a valid regular file name.
// `mode` contains the file mode. The file type bits must be S_IFREG. There is
// no restriction on the permission bits.
//
// On success, the file metadata is written to *stat.
//
// Returns:
//  0 on succes
//  ENOENT if the directory does not exist
//  ENOTDIR if `dir_ino` does not refer to a directory
//  EINVAL if the given `name` is invalid, or `mode` is not a regular file mode
//  EIO if a database operation failed
int sqlfs_mknod(struct sqlfs *sqlfs, ino_t dir_ino, const char *name, mode_t mode, struct stat *stat);

// Unlink a directory entry, and decrease the hardlink count of the referenced
// file by one. On success, the inode number of the unlinked file is written to
// *child_ino. Even if its hardlink count has dropped to 0, the file is not
// deleted until sqlfs_purge() is called.
//
// Returns:
//  0 on success
//  ENOENT if either the parent directory or the named file does not exist
//  EISDIR if the named file is actually a directory, not a file
//  EIO if a database operation failed
int sqlfs_unlink(struct sqlfs *sqlfs, ino_t dir_ino, const char *name, ino_t *child_ino);

// Delete metadata associated with the given file/directory, but only if its
// hardlink count is zero. (If the hardlink count is nonzero, this function does
// nothing).
//
// Returns:
//  0 on success
//  ENOENT if the inode doesn't exist
//  EIO if a database operation failed
int sqlfs_purge(struct sqlfs *sqlfs, ino_t ino);

// Attribute flags for use with sqlfs_set_attr() (see below).
#define SQLFS_SET_ATTR_MODE  (1 << 0)
#define SQLFS_SET_ATTR_UID   (1 << 1)
#define SQLFS_SET_ATTR_GID   (1 << 2)
#define SQLFS_SET_ATTR_MTIME (1 << 3)
#define SQLFS_SET_ATTR_SIZE  (1 << 4)
#define SQLFS_SET_ATTR_ALL   ((1 << 5) - 1)

// Changes the metadata for the given inode. The to_set bitmask must contain
// a combination of SQLFS_SET_ATTR_*-flags that indicate which fields in attr_in
// to apply.
//
// On success, the complete set of attributes is written to *attr_out (i.e.,
// including the old values of fields that weren't changed).
//
// Restrictions on fields:
//
//   - attr->st_mode: only the permission bits of the mode field can be changed.
//   - attr->st_size: changing the size results in truncation (if smaller than
//     the current size) or eager allocation (if greater than the current size).
//     Newly allocated bytes are set to zero (sparse files are not supported).
//     Only regular files can have their size changed.
//   - attr->st_time: timestamp in nanoseconds must be in range of a 64-bit
//     integer (which means approximately between the year 1823 and 2116).
//
// Returns:
//  0 on success
//  EINVAL if to_set contains invalid flags, or one of the fields is invalid.
//  ENOENT if the inode does not exist.
//  EIO if a database operation failed.
int sqlfs_set_attr(struct sqlfs *sqlfs, ino_t ino, const struct stat *attr_in, unsigned to_set, struct stat *attr_out);

// TODO: document
int sqlfs_read(struct sqlfs *sqlfs, ino_t ino, off_t off, char *buf, size_t size, size_t *size_out);

// TODO: document
int sqlfs_write(struct sqlfs *sqlfs, ino_t ino, off_t off, const char *buf, size_t size);

#endif
