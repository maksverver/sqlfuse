#ifndef SQLFUSE_H_INCLUDED
#define SQLFUSE_H_INCLUDED

#include <stdbool.h>

// This is supposed to be defined in the CFLAGS so that all compilation units
// see the same value.
#ifndef FUSE_USE_VERSION
#error FUSE_USE_VERSION undefined
#endif

#include "fuse_lowlevel.h"

// Versioning scheme: major.minor.patch
//
// The patch version is incremented when forward- and backward-compatible
// changes are made. That means software that differs only in the patch version
// can use each other's database files without problems. Changes like that
// include bug fixes, but also more substantial changes that don't change the
// database schema.
//
// The minor version is incremented when a backward-compatible change is made
// to the database schema. Backward-compatible means that a new version of the
// software is able to open an old database version, but not the other way
// around! That means that upgrading from a minor version to the next works, but
// downgrading to the previous does not. Typically, the database schema version
// (defined in sqlfs.h) is also incremented in that case, and an old version of
// the software would report an error when attempting to open a file with the
// new schema.
//
// The major version is incremented when a backward-incompatible change is made.
// That means that upgrading requires creating a new database file and copying
// the contents from an old version. This is very inconvenient for users, so
// this should be avoided if possible.
#define SQLFUSE_VERSION_MAJOR 0
#define SQLFUSE_VERSION_MINOR 0
#define SQLFUSE_VERSION_PATCH 0

// If set, calls to the functions defined in sqlfuse_ops will be printed to
// stderr (with an interesting subset of their arguments). This is mostly useful
// for debugging.
extern bool sqlfuse_tracing_enabled;

struct sqlfuse_userdata {
  struct sqlfs *sqlfs;
  struct intmap *lookups;
};

extern const struct fuse_lowlevel_ops sqlfuse_ops;

#endif /* ndef SQLFUSE_H_INCLUDED */
