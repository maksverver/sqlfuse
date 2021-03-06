// SQLFS is a filesystem stored in a SQLite3 database, optionally encrypted
// by a password, using the SQLCipher extension.
//
// All functions declared in this file are thread-compatible, NOT thread-safe!

#ifndef SQLFS_H_INCLUDED
#define SQLFS_H_INCLUDED

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define SQLFS_INO_NONE ((ino_t) 0)
#define SQLFS_INO_ROOT ((ino_t) 1)

enum sqlfs_open_mode {
  SQLFS_OPEN_MODE_READONLY,
  SQLFS_OPEN_MODE_READWRITE,
};

// Version of the database schema. (This value is mostly useful for debugging.)
//
// Whenever the schema of the database is changed:
//  - Add upgrade logic to sqlfs_open(), if possible.
//  - Increment the schema version number here.
//  - Update the version number in sqlfuse.h.
#define SQLFS_SCHEMA_VERSION 1

// The state for a single filesystem.
struct sqlfs;

// Options for sqlfs_create() and sqlfs_open().
struct sqlfs_options {
  // Path to the database file.
  const char *filepath;

  // Password to use. May be NULL to disable encryption.
  const char *password;

  // Effective uid, gid and umask to apply. These affect the permissions used
  // to create new files and directories.
  uid_t uid;
  gid_t gid;
  mode_t umask;

  // Number of iterations of PBKDF2 to apply (if password != NULL). This should
  // only be changed in tests. If 0, the SQLCipher default (typically 64,000)
  // will be used.
  int kdf_iter;
};

struct sqlcipher_version {
  int major;
  int minor;
  int patch;
};

// Retrieves the runtime version of the SQLCipher library.
//
// If the version could be retrieved, it is assigned to *result and 0 is
// returned. Otherwise, EIO is returned.
int sqlfs_get_sqlcipher_version(struct sqlcipher_version *result);

// Creates a new filesystem at the given path.
//
// Returns 0 if the filesystem was created successfully, EINVAL if options is
// NULL or contains an invalid value, or EIO otherwise.
int sqlfs_create(const struct sqlfs_options *options);

// Opens a filesystem at the given path.
//
// Returns NULL if the file could not be opened (e.g. invalid path, incorrect
// password, invalid file format, etc.). Otherwise, returns a pointer to the
// filesystem state, which must be released by calling sqlfs_close() later.
struct sqlfs *sqlfs_open(enum sqlfs_open_mode mode,
    const struct sqlfs_options *options);

// Upgrades the cipher parameters for a SQLCipher database to the latest
// version supported.
//
// Prints appropriate status/error messages during upgrade.
//
// Returns 0 if migration succeeded, EINVAL if `filepath` or `password` was
// NULL, ENOTSUP if the SQLCipher library version does not support migration, or
// EIO on any other failure.
int sqlfs_cipher_migrate(const char *filepath, const char *password);

// Releases the filesystem state. Afterwards, the state should not be used.
// `sqlfs` may be NULL. In that case, calling this function has no effect.
void sqlfs_close(struct sqlfs *sqlfs);

// Changes the password on an encrypted database.
//
// The database must have been created with a non-NULL password, and the new
// password must also not be NULL.
//
// Returns:
//   0 on success
//   EINVAL if new_password is NULL
//   EIO if a database operation failed
int sqlfs_rekey(struct sqlfs *sqlfs, const char *new_password);

// Vacuums the underlying database to reduce fragmentation and reclaim space.
//
// Returns 0 on success, or EIO if a database operation failed.
int sqlfs_vacuum(struct sqlfs *sqlfs);

// Returns the current blocksize, which will be used to create new files.
int sqlfs_get_blocksize(const struct sqlfs *sqlfs);

// Sets the new blocksize. Should be positive, and small enough to be able to
// allocate full blocks in memory. Usually, this is a power of 2.
void sqlfs_set_blocksize(struct sqlfs *sqlfs, int blocksize);

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
//  EIO if a database operation failed
int sqlfs_rmdir(struct sqlfs *sqlfs, ino_t dir_ino, const char *name, ino_t *child_ino);

// The sqlfs_dir_open(), sqlfs_dir_next() and sqlfs_dir_close() functions are
// used to read the contents of a directory. The first entry is always ".." and
// refers to the directory's parent directory. The following entries are given
// in lexicographical order. Note that the "." entry is not included at all!
//
// Within a single sqlfs instance, only one directory may be opened at a time,
// and no other operations (including sqlfs_close()) may be performed until
// it is closed. That means the user should typically keep a directory open
// for a very short time only, and treat the open, next, and close calls as a
// single operation.
//
// To enumerate the contents of large directories in smaller chunks, use the
// `start_name` argument to sqlfs_dir_open() to resume.
//
// Example usage:
//
//   struct sqlfs *sqlfs = ...;
//   struct ino_t dir_ino = 1;
//
//   const char *name;
//   ino_t ino;
//   mode_t mode;
//   sqlfs_dir_open(sqlfs, dir_ino, NULL);
//   while (sqlfs_dir_next(sqlfs, &name, &ino, &mode) == 0) {
//     printf("name=%s ino=%d mode=0%o\n", name, (int)ino, mode);
//   }
//   sqlfs_dir_close(sqlfs);
//

// Opens a directory for reading. `ino` must refer to a directory. `start_name`
// can be NULL (or equivalently, "" or "..") to start at the beginning, or the
// name of the next entry. The value of `start_name` is typically the last name
// returned by sqlfs_dir_next() before closing the directory. (It's possible
// that sqlfs_dir_next() returns a different entry, if the named entry has been
// deleted since.)
void sqlfs_dir_open(struct sqlfs *sqlfs, ino_t ino, const char *start_name);

// Reads the next directory entry, storing its name into *name, its inode
// number into *ino, and (only!) the file-type bits of its mode into *mode, and
// then returns 0.
//
// The buffer pointed to by *name is valid only until the next call to
// sqlfs_dir_next() or sqlfs_dir_close().
//
// If there are no more entries to return, this function sets *name to NULL,
// *ino to SQLFS_INO_NONE, and returns 0.
//
// Returns 0 on success, or EIO if a database operation failed.
int sqlfs_dir_next(struct sqlfs *sqlfs, const char **name, ino_t *ino, mode_t *mode);

// Closes a directory opened previously by sqlfs_dir_open().
void sqlfs_dir_close(struct sqlfs *sqlfs);

// Creates a new file in the given directory.
//
// `dir_ino` refers to the existing directory in which the file will be created.
// `name` contains the filename, which must be a valid regular file name.
// `mode` contains the file mode. The file type bits must be S_IFREG. Permission
// bits will be modified by the session's current umask. Sticky, setgid and setuid
// bits are not supported.
//
// On success, the file metadata is written to *stat.
//
// Returns:
//  0 on succes
//  ENOENT if the directory does not exist
//  ENOTDIR if `dir_ino` does not refer to a directory
//  EINVAL if the given `name` is invalid, or `mode` is not a valid file mode
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

// Unlinks the file or removes the directory with the given inode number, but
// only if its hardlink count is zero. (If the hardlink count is nonzero, this
// function does nothing, and returns successfully.)
//
// Returns:
//  0 on success
//  ENOENT if the inode doesn't exist
//  EIO if a database operation failed
int sqlfs_purge(struct sqlfs *sqlfs, ino_t ino);

// Unlinks all files and removes all directories whose hardlink count is zero.
// This is functionally equivalent to (but more efficient than) calling
// sqlfs_purge() for every inode in the database.
//
// Returns:
//  0 on success
//  EIO if a database operation failed
//  ENOTEMPTY if an unlinked directory is not empty (which should be impossible)
int sqlfs_purge_all(struct sqlfs *sqlfs);

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
//  EINVAL if to_set contains invalid flags, or one of the fields is invalid
//  ENOENT if the inode does not exist
//  EIO if a database operation failed
int sqlfs_set_attr(struct sqlfs *sqlfs, ino_t ino, const struct stat *attr_in, unsigned to_set, struct stat *attr_out);

// Reads data from a file into a buffer, starting at the given file offset.
//
// This routine always fills the buffer completely, unless the file is smaller
// than `off + size` bytes. On success, the number of bytes read is written to
// *size_out; a value smaller than `size` indicates that the end of the file was
// reached.
//
// Returns:
//  0 on success
//  ENOENT if the file does not exist
//  EISDIR if `ino` refers to a directory
//  EINVAL if `ino` refers to neither a regular file nor a directory
//  EIO if a database operation failed
int sqlfs_read(struct sqlfs *sqlfs, ino_t ino, off_t off, size_t size, char *buf, size_t *size_out);

// Writes data from a buffer into a file, starting at the given file offset,
// increasing the file size if necessary. Gaps are filled with zeroes.
//
// Returns:
//  0 on success
//  ENOENT if the file does not exist
//  EISDIR if `ino` refers to a directory
//  EINVAL if `ino` refers to neither a regular file nor a directory
//  EIO if a database operation failed
int sqlfs_write(struct sqlfs *sqlfs, ino_t ino, off_t off, size_t size, const char *buf);

// Renames a file/directory and/or moves it to a different directory.
//
// The old entry (described by old_parent_ino and old_name) must exist. The new
// entry (described by new_parent_ino and new_name) may or may not exist. If it
// exists, then the type of the old entry must match the type of the new entry,
// and the pre-existing entry will be unlinked (if it's a file) or removed (if
// it's an empty directory).
//
// If the old and new entries are hardlinks to the same file, then this function
// does nothing but returns succesfully. (This behavior is consistent with the
// behavior of rename().)
//
// On success, the inode number of the unlinked file or directory is written to
// *unlinked_ino. If there was no previous entry for the new name, then the
// value will be set to SQLFS_INO_NONE.
//
// IMPORTANT LIMITATION: it should not be possible to rename a directory to a
// subdirectory of itself (because this would create an unreachable cycle in the
// filesystem), but this function does not enforce this! That means that the
// caller must ensure this doesn't happen.
//
// Returns:
//  0 on success
//  EINVAL if either the old or the new names is not a regular file names
//  ENOENT if the old entry or the new directory does not exist
//  ENOTDIR if the old entry is a directory, but the new entry is not
//  EISDIR if the old entry is a file, but the new entry is a directory
//  ENOTEMPTY if the new entry is a directory which is not empty
//  EIO if a database operation failed
int sqlfs_rename(struct sqlfs *sqlfs,
    ino_t old_parent_ino, const char *old_name,
    ino_t new_parent_ino, const char *new_name, ino_t *unlinked_ino);

// Syncs filesystem to disk.
//
// If this function returns succesfully, then all changes made up to this point
// will be preserved in case of an unexpected system failure (for example, a
// sudden power loss). It's not necessary to call this to maintain consistency.
//
// Returns:
//  0 on success
//  EIO if a database operation failed
int sqlfs_sync(struct sqlfs *sqlfs);

#endif /* ndef SQLFS_H_INCLUDED */
