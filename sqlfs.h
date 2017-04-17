#ifndef SQLFS_H_INCLUDED
#define SQLFS_H_INCLUDED

/* TODO: documentation. */

/* NOTE: these functions are thread-compatible, but not thread-safe. */

struct sqlfs;

struct sqlfs *sqlfs_create(const char *filepath, const char *password);

void sqlfs_destroy(struct sqlfs *sqlfs);

#endif
