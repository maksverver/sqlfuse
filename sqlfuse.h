#ifndef SQLFUSE_H_INCLUDED
#define SQLFUSE_H_INCLUDED

/* This is supposed to be defined in the CFLAGS so that all compilation units
   see the same value. */
#ifndef FUSE_USE_VERSION
#error FUSE_USE_VERSION undefined
#endif

#include "fuse_lowlevel.h"

struct sqlfuse_userdata {
  struct sqlfs *sqlfs;
};

extern const struct fuse_lowlevel_ops sqlfuse_ops;

#endif /* ndef SQLFUSE_H_INCLUDED */
