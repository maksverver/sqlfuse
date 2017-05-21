// Implements a filesystem in an SQLite3 database.
//
// Naming conventios used in this file:
//
//  - Function names starting with "sqlfs_" indicate exported functions with a
//    declaration in sqlfs.h (which is also where they are documented).
//  - Helper function names starting with "sql_" indicate those functions
//    (mainly) execute SQL statements. They will be declared static.
//  - Other helper functions use no particular prefix. They will also be static.
//
// Conventions on error propagation:
//
// We use a mix of CHECK() failures (which abort execution immediately) and
// propagating errors back to the client.
//
//  - CHECK() failures are used for conditions that shouldn't happen and most
//    read-only query failures. The rationale is that we cannot reasonably
//    recover from those, and if the database is not readable, we cannot provide
//    any useful functionality, so we might as well crash immediately.
//  - Error propagation is used for failure of database updates. The rationale
//    is that it's possible to mount a read-only database, or for database
//    writes to fail (e.g., because the disk is full). We shouldn't crash, but
//    still support read-only functionality instead. Database write errors are
//    returned as errno EIO, unless otherwise specified.
//
// All database updates are performed within an exclusive transaction. This
// guarantees that if a CHECK() failure occurs halfway through an update, the
// transaction will be rolled back and the database will be left in a consistent
// state.

#include "sqlfs.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "logging.h"

#define ROOT_INO ((ino_t) 1)

// "Blocksize for IO" as returned in the st_blksize field of struct stat.
#define BLKSIZE 4096

#define NANOS_PER_SECOND 1000000000

enum statements {
  STMT_BEGIN_TRANSACTION,
  STMT_COMMIT_TRANSACTION,
  STMT_ROLLBACK_TRANSACTION,
  STMT_STAT,
  STMT_STAT_ENTRY,
  STMT_LOOKUP,
  STMT_UPDATE_NLINK,
  STMT_INSERT_METADATA,
  STMT_UPDATE_METADATA,
  STMT_DELETE_METADATA,
  STMT_INSERT_DIRENTRIES,
  STMT_COUNT_DIRENTRIES,
  STMT_DELETE_DIRENTRIES,
  STMT_DELETE_DIRENTRIES_BY_NAME,
  STMT_READ_DIRENTRIES,
  STMT_READ_DIRENTRIES_START,
  STMT_DELETE_FILEDATA,
  NUM_STATEMENTS };

// SQL strings for prepared statements. This could just be an array of strings,
// but including the `id` allows us to verify that array indices correspond one-
// to-one with the enumerators above, as a sanity-check.
//
// For each stament of the form STMT_XXX, we also define the query parameter
// indices (for use with sqlite3_bind()) as PARAM_XXX_YYY, and the projection
// column indices (for use with sqlite3_column()) as COL_XXX_ZZZ. Note that
// query parameters are indexed starting from 1, while projection columns are
// indexed from 0 instead!
static const struct Statement {
  int id;
  const char *sql;
} statements[NUM_STATEMENTS + 1] = {
  { STMT_BEGIN_TRANSACTION,
    "BEGIN TRANSACTION"},

  { STMT_COMMIT_TRANSACTION,
    "COMMIT TRANSACTION"},

  { STMT_ROLLBACK_TRANSACTION,
    "ROLLBACK TRANSACTION"},

  { STMT_STAT,
#define PARAM_STAT_INO   1
#define COL_STAT_INO     0
#define COL_STAT_MODE    1
#define COL_STAT_NLINK   2
#define COL_STAT_UID     3
#define COL_STAT_GID     4
#define COL_STAT_SIZE    5
#define COL_STAT_BLKSIZE 6
#define COL_STAT_MTIME   7
#define SELECT_METADATA "SELECT ino, mode, nlink, uid, gid, size, blksize, mtime FROM metadata"
    SELECT_METADATA " WHERE ino = ?" },

  { STMT_STAT_ENTRY,
#define PARAM_STAT_ENTRY_DIR_INO    1
#define PARAM_STAT_ENTRY_ENTRY_NAME 2
    SELECT_METADATA " INNER JOIN direntries ON ino = entry_ino WHERE dir_ino = ? AND entry_name = ?" },

  { STMT_LOOKUP,
#define PARAM_LOOKUP_DIR_INO    1
#define PARAM_LOOKUP_ENTRY_NAME 2
#define COL_LOOKUP_ENTRY_INO    0
#define COL_LOOKUP_ENTRY_TYPE   1
    "SELECT entry_ino, entry_type FROM direntries WHERE dir_ino = ? AND entry_name = ?" },

  { STMT_UPDATE_NLINK,
#define PARAM_UPDATE_NLINK_ADD_LINKS 1
#define PARAM_UPDATE_NLINK_INO 2
    "UPDATE metadata SET nlink = nlink + ? WHERE ino = ?" },

  { STMT_INSERT_METADATA,
#define PARAM_INSERT_METADATA_MODE    1
#define PARAM_INSERT_METADATA_NLINK   2
#define PARAM_INSERT_METADATA_UID     3
#define PARAM_INSERT_METADATA_GID     4
#define PARAM_INSERT_METADATA_MTIME   5
#define PARAM_INSERT_METADATA_SIZE    6
#define PARAM_INSERT_METADATA_BLKSIZE 7
    "INSERT INTO metadata(mode, nlink, uid, gid, mtime, size, blksize) VALUES (?, ?, ?, ?, ?, ?, ?)" },

  { STMT_UPDATE_METADATA,
#define PARAM_UPDATE_METADATA_MODE    1
#define PARAM_UPDATE_METADATA_UID     2
#define PARAM_UPDATE_METADATA_GID     3
#define PARAM_UPDATE_METADATA_MTIME   4
#define PARAM_UPDATE_METADATA_SIZE    5
#define PARAM_UPDATE_METADATA_INO     6
    "UPDATE metadata SET mode=?, uid=?, gid=?, mtime=?, size=? WHERE ino=?" },

  { STMT_DELETE_METADATA,
#define PARAM_DELETE_METADATA_INO 1
    "DELETE FROM metadata WHERE ino = ?" },

  { STMT_INSERT_DIRENTRIES,
#define PARAM_INSERT_DIRENTRIES_DIR_INO    1
#define PARAM_INSERT_DIRENTRIES_ENTRY_NAME 2
#define PARAM_INSERT_DIRENTRIES_ENTRY_INO  3
#define PARAM_INSERT_DIRENTRIES_ENTRY_TYPE 4
    "INSERT INTO direntries(dir_ino, entry_name, entry_ino, entry_type) VALUES (?, ?, ?, ?)" },

  { STMT_COUNT_DIRENTRIES,
#define PARAM_COUNT_DIRENTRIES_DIR_INO 1
#define COL_COUNT_DIRENTRIES_COUNT     0
    "SELECT count(entry_name) AS count FROM direntries WHERE dir_ino = ?" },

  { STMT_DELETE_DIRENTRIES,
#define PARAM_DELETE_DIRENTRIES_DIR_INO 1
    "DELETE FROM direntries WHERE dir_ino = ?" },

  { STMT_DELETE_DIRENTRIES_BY_NAME,
#define PARAM_DELETE_DIRENTRIES_BY_NAME_DIR_INO    1
#define PARAM_DELETE_DIRENTRIES_BY_NAME_ENTRY_NAME 2
    "DELETE FROM direntries WHERE dir_ino = ? AND entry_name = ?" },

  { STMT_READ_DIRENTRIES,
#define PARAM_READ_DIRENTRIES_DIR_INO  1
#define COL_READ_DIRENTRIES_ENTRY_NAME 0
#define COL_READ_DIRENTRIES_ENTRY_INO  1
#define COL_READ_DIRENTRIES_ENTRY_TYPE 2
#define SELECT_DIRENTRIES "SELECT entry_name, entry_ino, entry_type FROM direntries"
#define ORDER_DIRENTRIES "ORDER BY entry_name"
    SELECT_DIRENTRIES " WHERE dir_ino = ? " ORDER_DIRENTRIES },

  { STMT_READ_DIRENTRIES_START,
#define PARAM_READ_DIRENTRIES_START_DIR_INO    1
#define PARAM_READ_DIRENTRIES_START_ENTRY_NAME 2
    SELECT_DIRENTRIES " WHERE dir_ino = ? AND entry_name >= ? " ORDER_DIRENTRIES },

  { STMT_DELETE_FILEDATA,
#define PARAM_DELETE_FILEDATA_INO 1
    "DELETE FROM filedata WHERE ino = ?" },

  {-1, NULL}
};

struct sqlfs {
  sqlite3 *db;
  mode_t umask;
  uid_t uid;
  gid_t gid;
  sqlite3_stmt *stmt[NUM_STATEMENTS];
  sqlite3_stmt *dir_stmt;
};

enum name_kind { NAME_EMPTY, NAME_DOT, NAME_DOTDOT, NAME_REGULAR };

// NOTE: this doesn't identify names containing a slash (which are invalid too).
static enum name_kind name_kind(const char *name) {
  if (name == NULL) return NAME_EMPTY;
  if (name[0] == '\0') return NAME_EMPTY;
  if (name[0] != '.') return NAME_REGULAR;
  if (name[1] == '\0') return NAME_DOT;
  if (name[1] != '.') return NAME_REGULAR;
  if (name[2] == '\0') return NAME_DOTDOT;
  return NAME_REGULAR;
}

static bool prepare(sqlite3 *db, const char *sql, sqlite3_stmt **stmt) {
  if (sqlite3_prepare_v2(db, sql, -1, stmt, NULL) != SQLITE_OK) {
    fprintf(stderr, "Failed to prepare statement [%s]: %s\n", sql, sqlite3_errmsg(db));
    return false;
  }
  return true;
}

static void exec_sql(sqlite3 *db, const char *sql) {
  sqlite3_stmt *stmt = NULL;
  CHECK(prepare(db, sql, &stmt));
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
}

static void sql_begin_transaction(struct sqlfs *sqlfs) {
  sqlite3_stmt * const stmt = sqlfs->stmt[STMT_BEGIN_TRANSACTION];
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
}

static void sql_commit_transaction(struct sqlfs *sqlfs) {
  sqlite3_stmt * const stmt = sqlfs->stmt[STMT_COMMIT_TRANSACTION];
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
}

static void sql_rollback_transaction(struct sqlfs *sqlfs) {
  sqlite3_stmt * const stmt = sqlfs->stmt[STMT_ROLLBACK_TRANSACTION];
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
}

static bool sql_get_user_version(struct sqlfs *sqlfs, int64_t *user_version) {
  bool result = false;
  sqlite3_stmt *stmt = NULL;
  CHECK(prepare(sqlfs->db, "PRAGMA user_version", &stmt));
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    result = true;
    *user_version = sqlite3_column_int64(stmt, 0);
    CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  }
  sqlite3_finalize(stmt);
  return result;
}

static struct timespec current_timespec() {
  struct timespec tp;
  CHECK(clock_gettime(CLOCK_REALTIME, &tp) == 0);
  return tp;
}

// Converts a timespec structure to a 64-bit integer timestamp in nanoseconds.
// The result is clamped into the range [INT64_MIN:INT64_MAX] if necessary.
// This range allows dates between the year 1823 and 2116 to be represented.d
static int64_t timespec_to_nanos(const struct timespec *tp) {
  if (tp->tv_sec >= 0) {
    if (tp->tv_sec > INT64_MAX/NANOS_PER_SECOND ||
        INT64_MAX - (int64_t)tp->tv_sec * NANOS_PER_SECOND < tp->tv_nsec) {
      return INT64_MAX;
    }
  } else {  // tp->tv_sec < 0
    if (tp->tv_sec < INT64_MIN/NANOS_PER_SECOND ||
        INT64_MIN - (int64_t)tp->tv_sec * NANOS_PER_SECOND > tp->tv_nsec) {
      return INT64_MIN;
    }
  }
  return (int64_t)tp->tv_sec * NANOS_PER_SECOND + tp->tv_nsec;
}

static int64_t current_time_nanos() {
  struct timespec tp = current_timespec();
  return timespec_to_nanos(&tp);
}

static struct timespec nanos_to_timespec(int64_t nanos) {
  int64_t sec = nanos / NANOS_PER_SECOND;
  int64_t nsec = nanos % NANOS_PER_SECOND;
  if (nsec < 0) {
    sec -= 1;
    nsec += NANOS_PER_SECOND;
  }
  struct timespec res = {
    .tv_sec  = sec,
    .tv_nsec = nsec };
  return res;
}

// Creates the root directory in an empty, newly created filesystem.
//
// This is basically a special-case version of sqlfs_mkdir that doesn't use
// prepared statements so it can be called during initialization, before the
// schema has been committed.
static void create_root_directory(struct sqlfs *sqlfs) {
  sqlite3_stmt *stmt = NULL;

  const mode_t mode = (0777 &~ sqlfs->umask) | S_IFDIR;
  CHECK(prepare(sqlfs->db, "INSERT INTO metadata(ino, mode, nlink, uid, gid, mtime) VALUES (?, ?, ?, ?, ?, ?)", &stmt));
  CHECK(sqlite3_bind_int64(stmt, 1, ROOT_INO) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, 2, mode) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, 3, 1) == SQLITE_OK);  // nlink
  CHECK(sqlite3_bind_int64(stmt, 4, sqlfs->uid) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, 5, sqlfs->gid) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, 6, current_time_nanos()) == SQLITE_OK);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  CHECK(prepare(sqlfs->db, "INSERT INTO direntries(dir_ino, entry_name, entry_ino, entry_type) VALUES (?, ?, ?, ?)", &stmt));
  CHECK(sqlite3_bind_int64(stmt, 1, ROOT_INO) == SQLITE_OK);
  CHECK(sqlite3_bind_text(stmt, 2, "", 0, SQLITE_STATIC) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, 3, ROOT_INO) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, 4, mode >> 12) == SQLITE_OK);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
}

static void fill_stat(sqlite3_stmt *stmt, struct stat *stat) {
  memset(stat, 0, sizeof(*stat));
  stat->st_ino = sqlite3_column_int64(stmt, COL_STAT_INO);
  const mode_t mode = sqlite3_column_int64(stmt, COL_STAT_MODE);
  stat->st_mode = mode;
  stat->st_nlink = sqlite3_column_int64(stmt, COL_STAT_NLINK) + S_ISDIR(mode);
  stat->st_uid = sqlite3_column_int64(stmt, COL_STAT_UID);
  stat->st_gid = sqlite3_column_int64(stmt, COL_STAT_GID);
  // stat->st_rdev is kept zero.
  const int64_t size = sqlite3_column_int64(stmt, COL_STAT_SIZE);
  stat->st_size = size;
  // Blocksize for filesystem I/O
  stat->st_blksize = sqlite3_column_type(stmt, COL_STAT_BLKSIZE) == SQLITE_NULL
      ? BLKSIZE : sqlite3_column_int64(stmt, COL_STAT_BLKSIZE);
  // Size in 512 byte blocks. Unrelated to blocksize above!
  stat->st_blocks = (size + 511) >> 9;
  // atim/mtim/ctim are all set to the last modification timestamp.
  stat->st_atim = stat->st_mtim = stat->st_ctim =
    nanos_to_timespec(sqlite3_column_int64(stmt, COL_STAT_MTIME));
}

static int finish_stat_query(sqlite3_stmt *stmt, struct stat *stat) {
  int err = -1;
  int status = sqlite3_step(stmt);
  if (status == SQLITE_DONE) {
    err = ENOENT;
  } else if (status == SQLITE_ROW) {
    fill_stat(stmt, stat);
    CHECK(sqlite3_step(stmt) == SQLITE_DONE);
    err = 0;
  } else {
    LOG("[%s:%d] %s() status=%d\n", __FILE__, __LINE__, __func__, status);
    err = EIO;
  }
  CHECK(sqlite3_clear_bindings(stmt) == SQLITE_OK);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  CHECK(err >= 0);
  return err;
}

static int sql_stat_entry(struct sqlfs *sqlfs, ino_t dir_ino, const char *entry_name, struct stat *stat) {
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_STAT_ENTRY];
  CHECK(sqlite3_bind_int64(stmt, PARAM_STAT_ENTRY_DIR_INO, dir_ino) == SQLITE_OK);
  CHECK(sqlite3_bind_text(stmt, PARAM_STAT_ENTRY_ENTRY_NAME, entry_name, -1, SQLITE_STATIC) == SQLITE_OK);
  return finish_stat_query(stmt, stat);
}

// Allocates an inode number for a new file/directory with the given mode and
// link count. On success, *stat contains the generated attributes. In
// particular, the generated inode number is returned in stat->st_ino.
//
// Inode numbers may be re-used over the lifetime of the filesystem! (For
// example, if file A with inode 42 is deleted, it is possibly that file B is
// later created with the same inode number 42.)
//
// Returns 0 on success, or EIO if the database operation failed.
static int sql_insert_metadata(struct sqlfs *sqlfs, mode_t mode, nlink_t nlink, struct stat *stat) {
  memset(stat, 0, sizeof(*stat));
  stat->st_mode = mode;
  stat->st_nlink = nlink + S_ISDIR(mode);
  stat->st_uid = sqlfs->uid;
  stat->st_gid = sqlfs->gid;
  stat->st_blksize = BLKSIZE;
  stat->st_mtim = stat->st_ctim = stat->st_atim = current_timespec();
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_INSERT_METADATA];
  CHECK(sqlite3_bind_int64(stmt, PARAM_INSERT_METADATA_MODE, mode) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_INSERT_METADATA_NLINK, nlink) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_INSERT_METADATA_UID, sqlfs->uid) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_INSERT_METADATA_GID, sqlfs->gid) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_INSERT_METADATA_MTIME, timespec_to_nanos(&stat->st_mtim)) == SQLITE_OK);
  if (!S_ISDIR(mode)) {
    CHECK(sqlite3_bind_int64(stmt, PARAM_INSERT_METADATA_SIZE, 0) == SQLITE_OK);
    CHECK(sqlite3_bind_int64(stmt, PARAM_INSERT_METADATA_BLKSIZE, BLKSIZE) == SQLITE_OK);
  }

  int status = sqlite3_step(stmt);
  if (status != SQLITE_DONE) {
    LOG("[%s:%d] %s() status=%d\n", __FILE__, __LINE__, __func__, status);
  } else {
    stat->st_ino = sqlite3_last_insert_rowid(sqlfs->db);
    CHECK(stat->st_ino > 0);
  }
  CHECK(sqlite3_clear_bindings(stmt) == SQLITE_OK);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  return stat->st_ino > 0 ? 0 : EIO;
}

// Update the metadata for a file/directory identified by its inode number,
// which is passed in stat->st_ino.
//
// Only the following fields are updated: st_mode (permission bits only),
// st_uid, st_gid, st_mtime, st_size.
//
// File size can only be changed for files (not directories). The caller must
// make sure that the filedata table is updated separately.
//
// Returns:
//  0 on success
//  ENOENT if no metadata entry exists with the given inode number
//  EIO if writing to the database failed
static int sql_update_metadata(struct sqlfs *sqlfs, const struct stat *stat) {
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_UPDATE_METADATA];
  CHECK(sqlite3_bind_int64(stmt, PARAM_UPDATE_METADATA_INO, stat->st_ino) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_UPDATE_METADATA_MODE, stat->st_mode) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_UPDATE_METADATA_UID, stat->st_uid) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_UPDATE_METADATA_GID, stat->st_gid) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_UPDATE_METADATA_MTIME, timespec_to_nanos(&stat->st_mtim)) == SQLITE_OK);
  if (!S_ISDIR(stat->st_mode)) {
    CHECK(sqlite3_bind_int64(stmt, PARAM_UPDATE_METADATA_SIZE, stat->st_size) == SQLITE_OK);
  }

  int err = 0;
  int status = sqlite3_step(stmt);
  if (status != SQLITE_DONE) {
    LOG("[%s:%d] %s() status=%d\n", __FILE__, __LINE__, __func__, status);
    err = EIO;
  } else if (sqlite3_changes(sqlfs->db) == 0) {
    err = ENOENT;
  }
  CHECK(sqlite3_clear_bindings(stmt) == SQLITE_OK);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  return err;
}

// Deletes the metadata for a file/directory. Caller must make sure the contents
// of the file/directory are deleted separately!
//
// Returns 0 on success, or EIO if the database operation fails.
static int sql_delete_metadata(struct sqlfs *sqlfs, ino_t ino) {
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_DELETE_METADATA];
  CHECK(sqlite3_bind_int64(stmt, PARAM_DELETE_METADATA_INO, ino) == SQLITE_OK);
  int status = sqlite3_step(stmt);
  CHECK(sqlite3_clear_bindings(stmt) == SQLITE_OK);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  return status == SQLITE_DONE ? 0 : EIO;
}

// Updates the hardlink count for the given inode number, by adding add_links
// to the current link count. This function doesn't check that the new value is
// in range!
//
// Returns:
//  0 on success,
//  ENOENT if the ino does not exist
//  EIO for other SQLite errors
static int sql_update_nlink(struct sqlfs *sqlfs, ino_t ino, int64_t add_links) {
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_UPDATE_NLINK];
  CHECK(sqlite3_bind_int64(stmt, PARAM_UPDATE_NLINK_ADD_LINKS, add_links) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_UPDATE_NLINK_INO, ino) == SQLITE_OK);
  int err = -1;
  int status = sqlite3_step(stmt);
  if (status != SQLITE_DONE) {
    LOG("[%s:%d] %s(ino=%lld, add_links=%lld) status=%d\n", __FILE__, __LINE__,
        __func__, (long long) ino, (long long) add_links, status);
    err = EIO;
    goto finish;
  }
  int changes = sqlite3_changes(sqlfs->db);
  if (changes == 0) {
    err = ENOENT;
    goto finish;
  }
  CHECK(changes == 1);
  err = 0;
finish:
  CHECK(sqlite3_clear_bindings(stmt) == SQLITE_OK);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  CHECK(err >= 0);
  return err;
}

// Increments the hardlink count for the given inode number.
// See sql_update_nlink() for return values.
static int sql_inc_nlink(struct sqlfs *sqlfs, ino_t ino) {
  return sql_update_nlink(sqlfs, ino, +1);
}

// Decrements the hardlink count for the given inode number.
// See sql_update_nlink() for return values.
static int sql_dec_nlink(struct sqlfs *sqlfs, ino_t ino) {
  return sql_update_nlink(sqlfs, ino, -1);
}

// Retrieves an entry from the `direntry` table.
//
// Returns:
//  0 on success
//  ENOENT if the entry is not found (including if dir_ino didn't exist!)
//  EIO on SQLite error
static int sql_lookup(struct sqlfs *sqlfs, ino_t dir_ino, const char *entry_name,
    ino_t *child_ino, mode_t *child_mode) {
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_LOOKUP];
  CHECK(sqlite3_bind_int64(stmt, PARAM_LOOKUP_DIR_INO, dir_ino) == SQLITE_OK);
  CHECK(sqlite3_bind_text(stmt, PARAM_LOOKUP_ENTRY_NAME, entry_name, -1, SQLITE_STATIC) == SQLITE_OK);
  int status = sqlite3_step(stmt);
  int err = -1;
  if (status == SQLITE_ROW) {
    *child_ino = sqlite3_column_int64(stmt, COL_LOOKUP_ENTRY_INO);
    *child_mode = sqlite3_column_int64(stmt, COL_LOOKUP_ENTRY_TYPE) << 12;
    err = 0;
  } else if (status == SQLITE_DONE) {
    err = ENOENT;
  } else {
    LOG("[%s:%d] %s(dir_ino=%lld, name=[%s]) status=%d\n",
        __FILE__, __LINE__, __func__, (long long)dir_ino, entry_name, status);
    err = EIO;
  }
  CHECK(sqlite3_clear_bindings(stmt) == SQLITE_OK);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  CHECK(err >= 0);
  return err;
}

// Insert an entry into the direntries table. Only the file type bits of mode are stored!
// Entry name must not be "." or "..", but it may be the empty string, which corresponds
// with "..".
//
// Returns:
//  0 on success
//  EEXIST if the named entry already exists
//  EIO if another SQLite error occurred
static int sql_insert_direntries(struct sqlfs *sqlfs, ino_t dir_ino, const char *entry_name, ino_t entry_ino, mode_t entry_mode) {
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_INSERT_DIRENTRIES];
  CHECK(sqlite3_bind_int64(stmt, PARAM_INSERT_DIRENTRIES_DIR_INO, dir_ino) == SQLITE_OK);
  CHECK(sqlite3_bind_text(stmt, PARAM_INSERT_DIRENTRIES_ENTRY_NAME, entry_name, -1, SQLITE_STATIC) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_INSERT_DIRENTRIES_ENTRY_INO, entry_ino) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_INSERT_DIRENTRIES_ENTRY_TYPE, entry_mode >> 12) == SQLITE_OK);
  int status = sqlite3_step(stmt);
  CHECK(sqlite3_clear_bindings(stmt) == SQLITE_OK);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  switch (status) {
  case SQLITE_DONE:
    return 0;
  case SQLITE_CONSTRAINT:
    return EEXIST;
  default:
    LOG("[%s:%d] %s(dir_ino=%lld, entry_name=%s, entry_ino=%lld, entry_mode=0%o) status=%d\n",
        __FILE__, __LINE__, __func__, (long long)dir_ino, entry_name, (long long)entry_ino, entry_mode, status);
    return EIO;
  }
}

// Returns a count of the number of directory entries for the directory with
// inode number `dir_ino`, including the empty entry (corresponding to "..").
// That means the count is 1 for an empty directory, or 0 for non-existent
// directories (including files, which aren't directories).
static int64_t sql_count_direntries(struct sqlfs *sqlfs, ino_t dir_ino) {
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_COUNT_DIRENTRIES];
  CHECK(sqlite3_bind_int64(stmt, PARAM_COUNT_DIRENTRIES_DIR_INO, dir_ino) == SQLITE_OK);
  CHECK(sqlite3_step(stmt) == SQLITE_ROW);
  const int64_t result = sqlite3_column_int64(stmt, COL_COUNT_DIRENTRIES_COUNT);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  CHECK(sqlite3_clear_bindings(stmt) == SQLITE_OK);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  return result;
}

// Deletes the directory entry with the given name.
//
// This doesn't delete files/directories recursively. The caller must make sure
// that the file/directory being deleted is still linked elsewhere, or delete it
// separately.
//
// Returns 0 on success, or EIO if the database operation fails.
static int sql_delete_direntry(struct sqlfs *sqlfs, ino_t dir_ino, const char *entry_name) {
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_DELETE_DIRENTRIES_BY_NAME];
  CHECK(sqlite3_bind_int64(stmt, PARAM_DELETE_DIRENTRIES_BY_NAME_DIR_INO, dir_ino) == SQLITE_OK);
  CHECK(sqlite3_bind_text(stmt, PARAM_DELETE_DIRENTRIES_BY_NAME_ENTRY_NAME, entry_name, -1, SQLITE_STATIC) == SQLITE_OK);
  int status = sqlite3_step(stmt);
  CHECK(sqlite3_clear_bindings(stmt) == SQLITE_OK);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  return status == SQLITE_DONE ? 0 : EIO;
}

// Deletes all entries in the given directory. This includes the empty entry
// (corresponding to "..") so afterwards, the directory is in an invalid state,
// and its metadata should be removed separately.
//
// This doesn't delete files/directories recursively. The caller must make sure
// that the directory is empty before deleting its entries.
//
// Returns 0 on success, or EIO if the database operation fails.
static int sql_delete_direntries(struct sqlfs *sqlfs, ino_t dir_ino) {
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_DELETE_DIRENTRIES];
  CHECK(sqlite3_bind_int64(stmt, PARAM_DELETE_DIRENTRIES_DIR_INO, dir_ino) == SQLITE_OK);
  int status = sqlite3_step(stmt);
  CHECK(sqlite3_clear_bindings(stmt) == SQLITE_OK);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  return status == SQLITE_DONE ? 0 : EIO;
}

// Deletes the contents of the file identified by the given inode number.
// Caller must update/delete the file metadata separately!
//
// Returns 0 on success, or EIO if the database operation fails.
static int sql_delete_filedata(struct sqlfs *sqlfs, ino_t ino) {
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_DELETE_FILEDATA];
  CHECK(sqlite3_bind_int64(stmt, PARAM_DELETE_FILEDATA_INO, ino) == SQLITE_OK);
  int status = sqlite3_step(stmt);
  CHECK(sqlite3_clear_bindings(stmt) == SQLITE_OK);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  return status == SQLITE_DONE ? 0 : EIO;
}

struct sqlfs *sqlfs_create(
    const char *filepath, const char *password,
    mode_t umask, uid_t uid, gid_t gid) {
  struct sqlfs *sqlfs = calloc(1, sizeof(struct sqlfs));

  sqlfs->umask = umask;
  sqlfs->uid = uid;
  sqlfs->gid = gid;

  if (sqlite3_open_v2(filepath, &sqlfs->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) goto failed;

  /* TODO: set password */
  /* TODO: and other options (check doc!) */

  if (password != NULL) {
    sqlite3_key(sqlfs->db, password, strlen(password));

    /* Chosen to match Linux page size. */
    exec_sql(sqlfs->db, "PRAGMA cipher_page_size = 4096");
  }
  exec_sql(sqlfs->db, "PRAGMA foreign_keys = ON");
  /* exec_sql(sqlfs->db, "PRAGMA journal_mode = WAL"); */

  int64_t version = -1;

  if (!sql_get_user_version(sqlfs, &version)) {
    fprintf(stderr, "Failed to query database version: %s!\n"
        "This probably means the database is encrypted with a different password.\n",
        sqlite3_errmsg(sqlfs->db));
    goto failed;
  }

  if (version == 0) {
    /* Database was newly created. Initialize it. */
    exec_sql(sqlfs->db, "BEGIN TRANSACTION");
#define SQL_STATEMENT(sql) exec_sql(sqlfs->db, #sql);
#include "sqlfs_schema.h"
#undef SQL_STATEMENT
    exec_sql(sqlfs->db, "PRAGMA user_version = 1");
    create_root_directory(sqlfs);
    exec_sql(sqlfs->db, "COMMIT TRANSACTION");
  } else if (version != 1) {
    fprintf(stderr, "Wrong version number: %d (expected 1)\n", (int)version);
    goto failed;
  }

  // This must be done after the schema has been created.
  for (int i = 0; i < NUM_STATEMENTS; ++i) {
    CHECK(statements[i].id == i);
    if (!prepare(sqlfs->db, statements[i].sql, &sqlfs->stmt[i])) goto failed;
  }

  return sqlfs;

failed:
  sqlfs_destroy(sqlfs);
  return NULL;
}

void sqlfs_destroy(struct sqlfs *sqlfs) {
  // From the SQLite docs:
  //  "Applications should finalize all prepared statements, close all BLOB
  //   handles, and finish all sqlite3_backup objects associated with the
  //   sqlite3 object prior to attempting to close the object."
  // TODO: make sure that has actually happened!
  CHECK(sqlfs);
  CHECK(sqlfs->dir_stmt == NULL);
  for (int i = 0; i < NUM_STATEMENTS; ++i) {
    if (sqlfs->stmt[i]) {
      CHECK(sqlite3_finalize(sqlfs->stmt[i]) == SQLITE_OK);
    }
  }
  if (sqlfs->db) {
    CHECK(sqlite3_close(sqlfs->db) == SQLITE_OK);
  }
  free(sqlfs);
}

int sqlfs_stat(struct sqlfs *sqlfs, ino_t ino, struct stat *stat) {
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_STAT];
  CHECK(sqlite3_bind_int64(stmt, PARAM_STAT_INO, ino) == SQLITE_OK);
  return finish_stat_query(stmt, stat);
}

// Looks up a single directory entry. If succesful, the child inode number is
// returned in *ino, and (only!) the file type bits of the child inode in *mode.
//
// Return value:
//  0 on success
//  EINVAL if name is empty
//  ENOENT if not found
//  EIO on sqlite error
static int lookup(struct sqlfs *sqlfs, ino_t dir_ino, const char *name,
    ino_t *child_ino, mode_t *child_mode) {
  switch (name_kind(name)) {
    case NAME_DOT:
      // This assumes we already know dir_ino exists. Seems reasonable, but is
      // it guaranteed? (TODO: check the callers of this function!)
      *child_ino = dir_ino;
      *child_mode = S_IFDIR;
      return 0;

    case NAME_DOTDOT:
      return sql_lookup(sqlfs, dir_ino, "", child_ino, child_mode);

    case NAME_REGULAR:
      return sql_lookup(sqlfs, dir_ino, name, child_ino, child_mode);

    case NAME_EMPTY:
    default:
      return EINVAL;
  }
}

int sqlfs_stat_entry(struct sqlfs *sqlfs, ino_t dir_ino, const char *name,
    struct stat *stat) {
  switch (name_kind(name)) {
  case NAME_DOT:
    return sqlfs_stat(sqlfs, dir_ino, stat);

  case NAME_DOTDOT:
    return sql_stat_entry(sqlfs, dir_ino, "", stat);

  case NAME_REGULAR:
    return sql_stat_entry(sqlfs, dir_ino, name, stat);

  case NAME_EMPTY:
  default:
    return EINVAL;
  }
}

int sqlfs_mkdir(struct sqlfs *sqlfs, ino_t dir_ino, const char *name, mode_t mode,
    struct stat *stat) {
  memset(stat, 0, sizeof(*stat));

  if (name_kind(name) != NAME_REGULAR) {
    return EINVAL;
  }

  sql_begin_transaction(sqlfs);

  struct stat dir_stat;
  int err = sqlfs_stat(sqlfs, dir_ino, &dir_stat);
  if (err != 0) {
    goto finish;
  }
  if (!S_ISDIR(dir_stat.st_mode)) {
    err = ENOTDIR;
    goto finish;
  }
  err = sql_inc_nlink(sqlfs, dir_ino);
  if (err != 0) {
    goto finish;
  }
  mode = (mode & 0777 & ~sqlfs->umask) | S_IFDIR;
  err = sql_insert_metadata(sqlfs, mode, 1 /* nlink */, stat);
  if (err != 0) {
    goto finish;
  }
  // Insert child into parent directory.
  err = sql_insert_direntries(sqlfs, dir_ino, name, stat->st_ino, stat->st_mode);
  if (err != 0) {
    goto finish;
  }
  // Create reference from child to parent.
  err = sql_insert_direntries(sqlfs, stat->st_ino, "", dir_stat.st_ino, dir_stat.st_mode);
  if (err != 0) {
    goto finish;
  }
  err = 0;  // Succes

finish:
  if (err == 0) {
    sql_commit_transaction(sqlfs);
  } else {
    sql_rollback_transaction(sqlfs);
  }
  return err;
}

// Unlinks a file (if dir == false) or a directory (if dir == true).
//
// Used to implement sqlfs_rmdir() and sqlfs_unlink().
//
// Minor bug: this function should technicaly return ENOTDIR instead of ENOENT if
// dir_ino does not refer to a directory. (Perhaps this isn't an issue in practice, if
// FUSE verifies dir_ino refers to a directory before calling this function.)
static int remove_impl(struct sqlfs *sqlfs, ino_t dir_ino, const char *name, bool dir, ino_t *child_ino_out) {
  sql_begin_transaction(sqlfs);
  int err = -1;

  // Find the entry by its name in the given directory.
  ino_t child_ino = 0;
  mode_t child_mode = 0;
  err = lookup(sqlfs, dir_ino, name, &child_ino, &child_mode);
  if (err != 0) {
    // err is ENOENT, EINVAL or EIO
    goto rollback;
  }
  if (dir) {
    // Verify that entry refers to an empty subdirectory.
    if (!S_ISDIR(child_mode)) {
      err = ENOTDIR;
      goto rollback;
    }
    if (child_ino == ROOT_INO) {
      err = EBUSY;
      goto rollback;
    }
    if (child_ino == dir_ino) {
      err = EINVAL;
      goto rollback;
    }
    const int64_t entry_count = sql_count_direntries(sqlfs, child_ino);
    if (entry_count > 1) {
      err = ENOTEMPTY;
      goto rollback;
    }
    // If everything is consistent, the directory contains exactly one entry with
    // entry_name = "" and entry_ino = dir_ino: the link to the parent directory.
    CHECK(entry_count == 1);
    err = sql_delete_direntries(sqlfs, child_ino);
    if (err != 0) {
      // err is EIO
      goto rollback;
    }
    // Decrease parent directory link count.
    err = sql_dec_nlink(sqlfs, dir_ino);
    if (err != 0) {
      goto rollback;
    }
  } else {
    // Verify that entry refers to a file.
    if (S_ISDIR(child_mode)) {
      // Can only unlink files (or symlinks); not directories.
      err = EISDIR;
      goto rollback;
    }
  }

  // Unlink entry from parent directory & decrease its hardlink count.
  err = sql_delete_direntry(sqlfs, dir_ino, name);
  if (err != 0) {
    // err is EIO
    goto rollback;
  }
  err = sql_dec_nlink(sqlfs, child_ino);
  if (err != 0) {
    goto rollback;
  }

  CHECK(err == 0);
  *child_ino_out = child_ino;
  sql_commit_transaction(sqlfs);
  return 0;

rollback:
  CHECK(err > 0);
  sql_rollback_transaction(sqlfs);
  return err;
}

int sqlfs_rmdir(struct sqlfs *sqlfs, ino_t dir_ino, const char *name, ino_t *child_ino_out) {
  return remove_impl(sqlfs, dir_ino, name, true /* dir */, child_ino_out);
}

void sqlfs_dir_open(struct sqlfs *sqlfs, ino_t ino, const char *start_name) {
  CHECK(sqlfs->dir_stmt == NULL);
  sqlite3_stmt *stmt = NULL;
  switch (name_kind(start_name)) {
  case NAME_EMPTY:
  case NAME_DOT:
  case NAME_DOTDOT:
    stmt = sqlfs->stmt[STMT_READ_DIRENTRIES];
    CHECK(sqlite3_bind_int64(stmt, PARAM_READ_DIRENTRIES_DIR_INO, ino) == SQLITE_OK);
    break;

  case NAME_REGULAR:
    stmt = sqlfs->stmt[STMT_READ_DIRENTRIES_START];
    CHECK(sqlite3_bind_int64(stmt, PARAM_READ_DIRENTRIES_START_DIR_INO, ino) == SQLITE_OK);
    CHECK(sqlite3_bind_text(stmt, PARAM_READ_DIRENTRIES_START_ENTRY_NAME, start_name, -1, SQLITE_TRANSIENT) == SQLITE_OK);
  }
  CHECK(stmt);
  sqlfs->dir_stmt = stmt;
}

bool sqlfs_dir_next(struct sqlfs *sqlfs, const char **name, ino_t *ino, mode_t *mode) {
  sqlite3_stmt *stmt = sqlfs->dir_stmt;
  CHECK(stmt);
  int status = sqlite3_step(stmt);
  if (status == SQLITE_ROW) {
    const char *n = (const char *) sqlite3_column_text(stmt, COL_READ_DIRENTRIES_ENTRY_NAME);
    CHECK(n);
    *name = *n ? n : "..";
    *ino = sqlite3_column_int64(stmt, COL_READ_DIRENTRIES_ENTRY_INO);
    *mode = sqlite3_column_int64(stmt, COL_READ_DIRENTRIES_ENTRY_TYPE) << 12;
    return true;
  }
  if (status != SQLITE_DONE) {
    LOG("[%s:%d] %s() status=%d\n", __FILE__, __LINE__, __func__, status);
  }
  return false;
}

void sqlfs_dir_close(struct sqlfs *sqlfs) {
  sqlite3_stmt *stmt = sqlfs->dir_stmt;
  CHECK(stmt);
  CHECK(sqlite3_clear_bindings(stmt) == SQLITE_OK);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  sqlfs->dir_stmt = NULL;
}

int sqlfs_mknod(struct sqlfs *sqlfs, ino_t dir_ino, const char *name, mode_t mode, struct stat *stat) {
  memset(stat, 0, sizeof(*stat));

  if (name_kind(name) != NAME_REGULAR) {
    return EINVAL;
  }
  if (mode > S_IFMT || !S_ISREG(mode)) {
    return EINVAL;
  }

  sql_begin_transaction(sqlfs);

  struct stat dir_stat;
  int err = sqlfs_stat(sqlfs, dir_ino, &dir_stat);
  if (err != 0) {
    goto finish;
  }
  if (!S_ISDIR(dir_stat.st_mode)) {
    err = ENOTDIR;
    goto finish;
  }
  // Create file inode.
  err = sql_insert_metadata(sqlfs, mode, 1 /* nlink */, stat);
  if (err != 0) {
    goto finish;
  }
  // Insert file into parent directory.
  err = sql_insert_direntries(sqlfs, dir_ino, name, stat->st_ino, stat->st_mode);
  if (err != 0) {
    goto finish;
  }

finish:
  if (err == 0) {
    sql_commit_transaction(sqlfs);
  } else {
    sql_rollback_transaction(sqlfs);
  }
  return err;
}

int sqlfs_unlink(struct sqlfs *sqlfs, ino_t dir_ino, const char *name, ino_t *child_ino_out) {
  return remove_impl(sqlfs, dir_ino, name, false /* dir */, child_ino_out);
}

int sqlfs_purge(struct sqlfs *sqlfs, ino_t ino) {
  sql_begin_transaction(sqlfs);

  struct stat stat;
  int err = sqlfs_stat(sqlfs, ino, &stat);
  if (err != 0) {
    goto finish;
  }

  // For directories, stat() also counts the "." entry as a link, but if that's
  // the only link to a directory, it is not reachable and should be purged.
  if (stat.st_nlink > (S_ISDIR(stat.st_mode) ? 1 : 0)) {
    // Not ready to be purged yet. Return success immediately.
    err = 0;
    goto finish;
  }

  if (S_ISDIR(stat.st_mode)) {
    // If everything is consistent, the directory is already empty, so we don't
    // need to delete anything from the direntries table.
  } else {
    // Delete file contents from the filedata table.
    err = sql_delete_filedata(sqlfs, ino);
  }
  if (err != 0) {
    // err is EIO
    goto finish;
  }
  // Finally delete file/directory metadata.
  err = sql_delete_metadata(sqlfs, ino);

finish:
  if (err == 0) {
    sql_commit_transaction(sqlfs);
  } else {
    sql_rollback_transaction(sqlfs);
  }
  return err;
}

int sqlfs_set_attr(struct sqlfs *sqlfs, ino_t ino, const struct stat *attr_in, unsigned to_set, struct stat *attr_out) {

  CHECK(attr_in != attr_out);
  memset(attr_out, 0, sizeof(*attr_out));

  if (to_set != (to_set & SQLFS_SET_ATTR_ALL)) {
    // Unknown flags passed in `to_set`.
    LOG("[%s:%d] %s() to_set=%u\n", __FILE__, __LINE__, __func__, to_set);
    return EINVAL;
  }

  sql_begin_transaction(sqlfs);

  int err = sqlfs_stat(sqlfs, ino, attr_out);
  if (err != 0) {
    goto failure;
  }

  if (to_set & SQLFS_SET_ATTR_MODE) {
    // We only allow basic permission bits to be set. This excludes the sticky/
    // suid/sgid bits (07000).
    attr_out->st_mode = (attr_out->st_mode & ~0777) | (attr_in->st_mode & 0777);
  }
  if (to_set & SQLFS_SET_ATTR_UID) {
    attr_out->st_uid = attr_in->st_uid;
  }
  if (to_set & SQLFS_SET_ATTR_GID) {
    attr_out->st_gid = attr_in->st_gid;
  }
  if (to_set & SQLFS_SET_ATTR_MTIME) {
    // Convert to nanos and back, to guarantee the result is clamped in range.
    attr_out->st_mtim = nanos_to_timespec(timespec_to_nanos(&attr_in->st_mtim));
  }
  if (to_set & SQLFS_SET_ATTR_SIZE) {
    if (S_ISDIR(attr_out->st_mode)) {
      err = EISDIR;
      goto failure;
    }
    if (!S_ISREG(attr_out->st_mode)) {
      err = EINVAL;
      goto failure;
    }
    if (attr_out->st_size < 0) {
      err = EINVAL;
      goto failure;
    }
    if (!(to_set & SQLFS_SET_ATTR_MTIME)) {
      attr_out->st_mtim = current_timespec();
    }
    // TODO: update size. If changed, truncate/extend allocated size.
  }

  if (to_set != 0) {
    err = sql_update_metadata(sqlfs, attr_out);
    if (err != 0) {
      goto failure;
    }
  }

  CHECK(err == 0);
  sql_commit_transaction(sqlfs);
  return 0;

failure:
  CHECK(err != 0);
  sql_rollback_transaction(sqlfs);
  return err;
}

int sqlfs_read(struct sqlfs *sqlfs, ino_t ino, off_t off, char *buf, size_t size, size_t *size_read) {
  return ENOSYS;
}

int sqlfs_write(struct sqlfs *sqlfs, ino_t ino, off_t off, const char *buf, size_t size) {
  return ENOSYS;
}
