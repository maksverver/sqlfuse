// SQLFS is a filesystem stored in a SQLite3 database, optionally encrypted
// by a password, using the SQLCipher extension.
//
// All functions declared in this file are thread-compatible, NOT thread-safe!

#ifndef SQLFS_H_INCLUDED
#define SQLFS_H_INCLUDED

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
//  TODO -- describe other error conditions!
int sqlfs_mkdir(struct sqlfs *sqlfs, ino_t dir_ino, const char *name, mode_t mode,
    struct stat *stat);

// Removes subdirectory.
//
// Returns:
//  0 on success
//  EINVAL if the name is invalid ("." or empty)
//  ENOENT if the directory to be removed is not found
//  ENOTDIR if the named entry does not refer to a directory
//  EBUSY if the named entry refers to the root directory
//  ENOTEMPTY if the named directory is not empty
//  EIO if a database operation fails
int sqlfs_rmdir(struct sqlfs *sqlfs, ino_t dir_ino, const char *name);

#endif
