// SQLFS is a filesystem stored in a SQLite3 database, optionally encrypted
// by a password, using the SQLCipher extension.
//
// All functions declared in this file are thread-compatible, NOT thread-safe!

#ifndef SQLFS_H_INCLUDED
#define SQLFS_H_INCLUDED

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
//  mode: file permission bits of root directory (if creating a new filesystem)
//  uid: owner id of root directory (if creating a new filesystem)
//  gid: group id of root directory (if creating a new filesystem)
struct sqlfs *sqlfs_create(
    const char *filepath, const char *password,
    mode_t mode, uid_t uid, gid_t gid);

// Destroys the filesystem state. Afterwards, the state should not be used.
void sqlfs_destroy(struct sqlfs *sqlfs);

#endif
