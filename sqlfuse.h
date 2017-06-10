#ifndef SQLFUSE_H_INCLUDED
#define SQLFUSE_H_INCLUDED

#include <stdbool.h>

// This is supposed to be defined in the CFLAGS so that all compilation units
// see the same value.
#ifndef FUSE_USE_VERSION
#error FUSE_USE_VERSION undefined
#endif

#include "fuse_lowlevel.h"

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
