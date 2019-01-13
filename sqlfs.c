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
//
// We use savepoints to allow transactions to be nested. Each call to
// sql_savepoint() must be paired with a later call to sql_release_savepoint().
// If an operation failed and the changes made in the transaction should be
// rolled back, call sql_rollback_to_savepoint() to discard those changes, just
// before calling sql_release_savepoint().

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

// Log a message, prefixed with a file name and line number.
// Used for debug-logging throughout this file.
#define DLOG(fmt, ...) LOG("[%s:%d] " fmt, __FILE__, __LINE__, __VA_ARGS__)

#define NANOS_PER_SECOND 1000000000

// Minimum/maximum supported SQLCipher major version.
//
// The maximum is also capped by the current SQLCipher library version (see
// sqlfs_get_sqlcipher_version()) so in principle we don't need the upper limit,
// but this way we won't unintentionally create files incompatible with v4.
#define MIN_CIPHER_COMPAT 3
#define MAX_CIPHER_COMPAT 4

enum statements {
  STMT_SAVEPOINT,
  STMT_RELEASE_SAVEPOINT,
  STMT_ROLLBACK_TO_SAVEPOINT,
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
  STMT_REPARENT_DIRECTORY,
  STMT_READ_DIRENTRIES,
  STMT_READ_DIRENTRIES_START,
  STMT_RENAME,
  STMT_READ_FILEDATA,
  STMT_UPDATE_FILEDATA,
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
static const struct statement {
  int id;
  const char *sql;
} statements[NUM_STATEMENTS] = {
  { STMT_SAVEPOINT,
    "SAVEPOINT tx" },

  { STMT_RELEASE_SAVEPOINT,
    "RELEASE SAVEPOINT tx" },

  { STMT_ROLLBACK_TO_SAVEPOINT,
    "ROLLBACK TO SAVEPOINT tx" },

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

  { STMT_REPARENT_DIRECTORY,
#define PARAM_REPARENT_DIRECTORY_NEW_PARENT_INO 1
#define PARAM_REPARENT_DIRECTORY_CHILD_INO      2
    "UPDATE direntries SET entry_ino = ? WHERE dir_ino = ? AND entry_name = ''" },

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

  { STMT_RENAME,
#define PARAM_RENAME_NEW_DIR_INO    1
#define PARAM_RENAME_NEW_ENTRY_NAME 2
#define PARAM_RENAME_OLD_DIR_INO    3
#define PARAM_RENAME_OLD_ENTRY_NAME 4
    "UPDATE direntries SET dir_ino = ?, entry_name = ? WHERE dir_ino = ? AND entry_name = ?" },

  { STMT_READ_FILEDATA,
#define PARAM_READ_FILEDATA_INO      1
#define PARAM_READ_FILEDATA_FROM_IDX 2
#define PARAM_READ_FILEDATA_COUNT    3
#define COL_READ_FILEDATA_IDX  0
#define COL_READ_FILEDATA_DATA 1
    "SELECT idx, data FROM filedata WHERE ino = ? AND idx >= ? ORDER BY idx LIMIT ?" },

  { STMT_UPDATE_FILEDATA,
#define PARAM_UPDATE_FILEDATA_INO  1
#define PARAM_UPDATE_FILEDATA_IDX  2
#define PARAM_UPDATE_FILEDATA_DATA 3
    "INSERT OR REPLACE INTO filedata(ino, idx, data) VALUES (?, ?, ?)" },

  { STMT_DELETE_FILEDATA,
#define PARAM_DELETE_FILEDATA_INO      1
#define PARAM_DELETE_FILEDATA_FROM_IDX 2
    "DELETE FROM filedata WHERE ino = ? AND idx >= ?" },
};

struct sqlfs {
  sqlite3 *db;
  mode_t umask;
  uid_t uid;
  gid_t gid;
  int blocksize;
  sqlite3_stmt *stmt[NUM_STATEMENTS];
  sqlite3_stmt *dir_stmt;
  bool wal_enabled;
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

static int int_min(int x, int y) {
  return x < y ? x : y;
}

static int64_t int64_min(int64_t x, int64_t y) {
  return x < y ? x : y;
}

static int64_t int64_max(int64_t x, int64_t y) {
  return x > y ? x : y;
}

// Compares two character pointers for equality. Either argument may be NULL.
// The arguments are equal if they are both NULL, or they are both non-NULL and
// they have the same character contents.
static bool strings_equal(const char *s, const char *t) {
  return s == t || (s != NULL && t != NULL && strcmp(s, t) == 0);
}

static bool prepare(sqlite3 *db, const char *sql, sqlite3_stmt **stmt) {
  if (sqlite3_prepare_v2(db, sql, -1, stmt, NULL) != SQLITE_OK) {
    fprintf(stderr, "Failed to prepare statement [%s]: %s\n", sql, sqlite3_errmsg(db));
    return false;
  }
  return true;
}

// Executes an SQL statement that returns no values.
static bool exec_sql(sqlite3 *db, const char *sql) {
  sqlite3_stmt *stmt = NULL;
  if (!prepare(db, sql, &stmt)) {
    return false;
  }
  int status = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (status == SQLITE_DONE) {
    return true;
  }
  DLOG("%s(sql=[%s]) status=%d\n", __func__, sql, status);
  return false;
}

// Executes a PRAGMA sql statement which is expected to return a value.
//
// (For PRAGMA statements that don't return a value, use exec_sql() instead.)
//
// Returns true if the statement succeeded and returned exactly one row where
// the first column has a string value equal to `expected` (which may be NULL,
// in which case the received value should also be NULL).
static bool exec_pragma(sqlite3 *db, const char *sql, const char *expected) {
  sqlite3_stmt *stmt = NULL;
  if (!prepare(db, sql, &stmt)) {
    return false;
  }
  bool success = false;
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    DLOG("%s(sql=[%s]) failed (PRAGMA not supported by this version?)\n", __func__, sql);
  } else {
    const char *received = (const char*)sqlite3_column_text(stmt, 0);
    if (!strings_equal(expected, received)) {
      DLOG("%s(sql=[%s]) expected [%s] != received [%s]\n", __func__, sql,
          expected != NULL ? expected : "<null>",
          received != NULL ? received : "<null>");
    } else if (sqlite3_step(stmt) != SQLITE_DONE) {
      DLOG("%s(sql=[%s]) failed\n", __func__, sql);
    } else {
      success = true;
    }
  }
  sqlite3_finalize(stmt);
  return success;
}

static void sql_savepoint(struct sqlfs *sqlfs) {
  sqlite3_stmt * const stmt = sqlfs->stmt[STMT_SAVEPOINT];
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_reset(stmt);
}

static void sql_release_savepoint(struct sqlfs *sqlfs) {
  sqlite3_stmt * const stmt = sqlfs->stmt[STMT_RELEASE_SAVEPOINT];
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_reset(stmt);
}

static void sql_rollback_to_savepoint(struct sqlfs *sqlfs) {
  sqlite3_stmt * const stmt = sqlfs->stmt[STMT_ROLLBACK_TO_SAVEPOINT];
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_reset(stmt);
}

int sqlfs_get_sqlcipher_version(struct sqlcipher_version *result) {
  bool success = false;
  sqlite3 *db;
  CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
  sqlite3_stmt *stmt = NULL;
  CHECK(prepare(db, "PRAGMA cipher_version", &stmt));
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *text = (const char*)sqlite3_column_text(stmt, 0);
    struct sqlcipher_version version = {0, 0, 0};
    if (text != NULL && sscanf(text, "%d.%d.%d", &version.major, &version.minor, &version.patch) == 3) {
      *result = version;
      success = true;
    }
    CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  }
  sqlite3_finalize(stmt);
  CHECK(sqlite3_close(db) == SQLITE_OK);
  return success ? 0 : EIO;
}

// Retrieves the runtime version number of the SQLCipher library and returns
// true if the major version is at least MIN_CIPHER_COMPAT. Otherwise, it
// prints an error message and returns false.
static bool get_supported_sqlcipher_version(struct sqlcipher_version *result) {
  if (sqlfs_get_sqlcipher_version(result) != 0) {
    return false;
  }
  if (result->major < MIN_CIPHER_COMPAT) {
    fprintf(stderr, "Unsupported SQLCipher version: %d.%d.%d (minimum: %d.0.0)\n",
        result->major, result->minor, result->patch, MIN_CIPHER_COMPAT);
    return false;
  }
  return true;
}

static int get_user_version_with_status(sqlite3 *db, int64_t *result) {
  sqlite3_stmt *stmt = NULL;
  CHECK(prepare(db, "PRAGMA user_version", &stmt));
  int status = sqlite3_step(stmt);
  if (status == SQLITE_ROW) {
    *result = sqlite3_column_int64(stmt, 0);
    CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  }
  sqlite3_finalize(stmt);
  return status;
}

static int64_t get_user_version(sqlite3 *db) {
  int64_t result;
  return get_user_version_with_status(db, &result) == SQLITE_ROW ? result : -1;
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
static bool create_root_directory(sqlite3 *db, mode_t umask, uid_t uid, gid_t gid) {
  const mode_t mode = (0777 &~ umask) | S_IFDIR;
  {
    sqlite3_stmt *stmt = NULL;
    CHECK(prepare(db, "INSERT INTO metadata(ino, mode, nlink, uid, gid, mtime) VALUES (?, ?, ?, ?, ?, ?)", &stmt));
    CHECK(sqlite3_bind_int64(stmt, 1, SQLFS_INO_ROOT) == SQLITE_OK);
    CHECK(sqlite3_bind_int64(stmt, 2, mode) == SQLITE_OK);
    CHECK(sqlite3_bind_int64(stmt, 3, 1) == SQLITE_OK);  // nlink
    CHECK(sqlite3_bind_int64(stmt, 4, uid) == SQLITE_OK);
    CHECK(sqlite3_bind_int64(stmt, 5, gid) == SQLITE_OK);
    CHECK(sqlite3_bind_int64(stmt, 6, current_time_nanos()) == SQLITE_OK);
    int status = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (status != SQLITE_DONE) {
      return false;
    }
  }
  {
    sqlite3_stmt *stmt = NULL;
    CHECK(prepare(db, "INSERT INTO direntries(dir_ino, entry_name, entry_ino, entry_type) VALUES (?, ?, ?, ?)", &stmt));
    CHECK(sqlite3_bind_int64(stmt, 1, SQLFS_INO_ROOT) == SQLITE_OK);
    CHECK(sqlite3_bind_text(stmt, 2, "", 0, SQLITE_STATIC) == SQLITE_OK);
    CHECK(sqlite3_bind_int64(stmt, 3, SQLFS_INO_ROOT) == SQLITE_OK);
    CHECK(sqlite3_bind_int64(stmt, 4, mode >> 12) == SQLITE_OK);
    int status = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (status != SQLITE_DONE) {
      return false;
    }
  }
  return true;
}

static void fill_stat(sqlite3_stmt *stmt, struct stat *stat, int default_blocksize) {
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
      ? default_blocksize : sqlite3_column_int64(stmt, COL_STAT_BLKSIZE);
  // Size in 512 byte blocks. Unrelated to blocksize above!
  stat->st_blocks = (size + 511) >> 9;
  // atim/mtim/ctim are all set to the last modification timestamp.
  stat->st_atim = stat->st_mtim = stat->st_ctim =
    nanos_to_timespec(sqlite3_column_int64(stmt, COL_STAT_MTIME));
}

static int finish_stat_query(sqlite3_stmt *stmt, struct stat *stat, int default_blocksize) {
  int err = -1;
  int status = sqlite3_step(stmt);
  if (status == SQLITE_DONE) {
    err = ENOENT;
  } else if (status == SQLITE_ROW) {
    fill_stat(stmt, stat, default_blocksize);
    CHECK(sqlite3_step(stmt) == SQLITE_DONE);
    err = 0;
  } else {
    DLOG("%s() status=%d\n", __func__, status);
    err = EIO;
  }
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  CHECK(err >= 0);
  return err;
}

static int sql_stat_entry(struct sqlfs *sqlfs, ino_t dir_ino, const char *entry_name, struct stat *stat) {
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_STAT_ENTRY];
  CHECK(sqlite3_bind_int64(stmt, PARAM_STAT_ENTRY_DIR_INO, dir_ino) == SQLITE_OK);
  CHECK(sqlite3_bind_text(stmt, PARAM_STAT_ENTRY_ENTRY_NAME, entry_name, -1, SQLITE_STATIC) == SQLITE_OK);
  return finish_stat_query(stmt, stat, sqlfs->blocksize);
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
  stat->st_blksize = sqlfs->blocksize;
  stat->st_mtim = stat->st_ctim = stat->st_atim = current_timespec();
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_INSERT_METADATA];
  CHECK(sqlite3_bind_int64(stmt, PARAM_INSERT_METADATA_MODE, mode) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_INSERT_METADATA_NLINK, nlink) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_INSERT_METADATA_UID, sqlfs->uid) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_INSERT_METADATA_GID, sqlfs->gid) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_INSERT_METADATA_MTIME, timespec_to_nanos(&stat->st_mtim)) == SQLITE_OK);
  if (!S_ISDIR(mode)) {
    CHECK(sqlite3_bind_int64(stmt, PARAM_INSERT_METADATA_SIZE, 0) == SQLITE_OK);
    CHECK(sqlite3_bind_int64(stmt, PARAM_INSERT_METADATA_BLKSIZE, sqlfs->blocksize) == SQLITE_OK);
  }

  int status = sqlite3_step(stmt);
  if (status != SQLITE_DONE) {
    DLOG("%s() status=%d\n", __func__, status);
  } else {
    stat->st_ino = sqlite3_last_insert_rowid(sqlfs->db);
    CHECK(stat->st_ino > 0);
  }
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
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
    DLOG("%s() status=%d\n", __func__, status);
    err = EIO;
  } else if (sqlite3_changes(sqlfs->db) == 0) {
    err = ENOENT;
  }
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
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
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
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
    DLOG("%s(ino=%lld, add_links=%lld) status=%d\n",
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
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
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
    DLOG("%s(dir_ino=%lld, name=[%s]) status=%d\n",
        __func__, (long long)dir_ino, entry_name, status);
    err = EIO;
  }
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
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
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  switch (status) {
  case SQLITE_DONE:
    return 0;
  case SQLITE_CONSTRAINT:
    return EEXIST;
  default:
    DLOG("%s(dir_ino=%lld, entry_name=%s, entry_ino=%lld, entry_mode=0%o) status=%d\n",
        __func__, (long long)dir_ino, entry_name, (long long)entry_ino, entry_mode, status);
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
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
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
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  return status == SQLITE_DONE ? 0 : EIO;
}

// Changes the parent directory reference for the child directory with the given
// inode number.
//
// The caller must ensure that child_ino is an existing directory, and that no
// cycles are introduced by the reparenting operation. This function only
// updates the direntries table. It is the caller's responsibility to update the
// link count for the directories.
//
// Returns 0 on succes, or EIO if a database operation fails.
static int sql_reparent_directory(struct sqlfs *sqlfs, ino_t child_ino, ino_t new_parent_ino) {
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_REPARENT_DIRECTORY];
  CHECK(sqlite3_bind_int64(stmt, PARAM_REPARENT_DIRECTORY_NEW_PARENT_INO, new_parent_ino) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_REPARENT_DIRECTORY_CHILD_INO, child_ino) == SQLITE_OK);
  int status = sqlite3_step(stmt);
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  return status == SQLITE_DONE && sqlite3_changes(sqlfs->db) == 1 ? 0 : EIO;
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
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  return status == SQLITE_DONE ? 0 : EIO;
}

// Renames a directory entry and/or moves it a different directory.
//
// The caller must ensure that:
//
//  - old_dir_ino and new_dir_ino refer to directories
//  - the entry identified by (old_dir_ino, old_entry_name) exists
//  - the entry identified by (new_dir_ino, new_entry_name) does not exist
//  - no cycles are introduced by moving the old entry into the new directory
//
// Returns 0 on success, or EIO if the database operation fails (possibly
// because one of the constraints outlined above was violated).
static int sql_rename(struct sqlfs *sqlfs,
    ino_t old_dir_ino, const char *old_entry_name,
    ino_t new_dir_ino, const char *new_entry_name) {
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_RENAME];
  CHECK(sqlite3_bind_int64(stmt, PARAM_RENAME_NEW_DIR_INO, new_dir_ino) == SQLITE_OK);
  CHECK(sqlite3_bind_text(stmt, PARAM_RENAME_NEW_ENTRY_NAME, new_entry_name, -1, SQLITE_STATIC) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_RENAME_OLD_DIR_INO, old_dir_ino) == SQLITE_OK);
  CHECK(sqlite3_bind_text(stmt, PARAM_RENAME_OLD_ENTRY_NAME, old_entry_name, -1, SQLITE_STATIC) == SQLITE_OK);
  int status = sqlite3_step(stmt);
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  return status == SQLITE_DONE && sqlite3_changes(sqlfs->db) == 1 ? 0 : EIO;
}

// Reads filedata bytes from the given file into a buffer.
//
// If there is not enough file data to fill the buffer, this function will fail!
// It is assumed that the caller knows exactly how large the file is, and adjust
// its read calls accordingly.
//
// `blocksize` must be a positive integer: the blocksize of the file identified
// by `ino`. `offset` and `size` must be nonnegative integers. `buf` must point
// to a buffer at least `size` bytes long.
//
// Returns 0 on success, or EIO if the database operation fails.
static int sql_read_filedata(struct sqlfs *sqlfs, ino_t ino, int64_t blocksize, int64_t offset, int64_t size, char *buf) {
  CHECK(blocksize > 0);
  CHECK(offset >= 0);
  CHECK(size >= 0);
  if (size == 0) {
    return 0;
  }
  int err = 0;
  // Range of blocks to read: from block_idx_begin (inclusive) to block_idx_end (exclusive).
  int64_t block_idx_begin = offset/blocksize;
  int64_t block_idx_end = (offset + size + blocksize - 1)/blocksize;
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_READ_FILEDATA];
  CHECK(sqlite3_bind_int64(stmt, PARAM_READ_FILEDATA_INO, ino) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_READ_FILEDATA_FROM_IDX, block_idx_begin) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_READ_FILEDATA_COUNT, block_idx_end - block_idx_begin) == SQLITE_OK);
  // Fill buffer by reading a sequence of chunks. Each chunk starts at a block
  // boundary and is at most `blocksize` bytes long (but the last chunk may be
  // shorter than that).
  for (int64_t i = block_idx_begin; i < block_idx_end; ++i) {
    int64_t chunk_offset = i*blocksize;
    int64_t chunk_size = int64_min(offset + size - chunk_offset, blocksize);
    int status = sqlite3_step(stmt);
    if (status != SQLITE_ROW) {
      DLOG("%s() status=%d\n", __func__, status);
      err = EIO;
      goto finish;
    }
    int64_t block_idx = sqlite3_column_int64(stmt, COL_READ_FILEDATA_IDX);
    if (block_idx != i) {
      DLOG("%s() ino=%lld incorrect block index! expected: %lld received: %lld",
          __func__, (long long)ino, (long long)i, (long long)block_idx);
      err = EIO;
      goto finish;
    }
    const char *data_ptr = sqlite3_column_blob(stmt, COL_READ_FILEDATA_DATA);
    CHECK(data_ptr != NULL);
    int64_t data_size = sqlite3_column_bytes(stmt, COL_READ_FILEDATA_DATA);
    if (data_size < chunk_size) {
      DLOG("%s() ino=%lld idx=%lld incorrect block size! expected: %lld received: %lld",
          __func__, (long long)ino, (long long)i, (long long)chunk_size, (long long)data_size);
      err = EIO;
      goto finish;
    }
    if (chunk_offset < offset) {
      // First chunk. Only copy the part overlapping the range to be read.
      memcpy(buf, data_ptr + (offset - chunk_offset), chunk_size - (offset - chunk_offset));
    } else {
      memcpy(buf + (chunk_offset - offset), data_ptr, chunk_size);
    }
  }
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);

finish:
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  return err;
}

// Writes one block of data to the filedata table.
//
// Returns 0 on success, or EIO if the database operation fails.
static int sql_update_filedata(struct sqlfs *sqlfs, ino_t ino, int64_t block_idx, const char *block_data, size_t block_size) {
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_UPDATE_FILEDATA];
  CHECK(sqlite3_bind_int64(stmt, PARAM_UPDATE_FILEDATA_INO, ino) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_UPDATE_FILEDATA_IDX, block_idx) == SQLITE_OK);
  CHECK(sqlite3_bind_blob64(stmt, PARAM_UPDATE_FILEDATA_DATA, block_data, block_size, SQLITE_STATIC) == SQLITE_OK);
  int status = sqlite3_step(stmt);
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  return status == SQLITE_DONE ? 0 : EIO;
}

// Deletes the filedata blocks for the file with the given inode number,
// starting from the block with index from_idx. (Consequently, if from_idx <= 0,
// then all filedata blocks are deleted.) The caller must update/delete the file
// metadata separately.
//
// Returns 0 on success, or EIO if the database operation fails.
static int sql_delete_filedata(struct sqlfs *sqlfs, ino_t ino, int64_t from_idx) {
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_DELETE_FILEDATA];
  CHECK(sqlite3_bind_int64(stmt, PARAM_DELETE_FILEDATA_INO, ino) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, PARAM_DELETE_FILEDATA_FROM_IDX, from_idx) == SQLITE_OK);
  int status = sqlite3_step(stmt);
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  return status == SQLITE_DONE ? 0 : EIO;
}

// Enable exclusive locking mode on the database. This has two effects:
//  1. Prevents others from accessing the same filesystem.
//  2. Allows WAL journaling without using shared memory files.
static bool enable_exclusive(sqlite3 *db) {
  return exec_pragma(db, "PRAGMA locking_mode = exclusive", "exclusive");
}

// Enable secure delete on the database. This ensures that deleted data is
// overwritten with zeroes, which prevents recovering deleted data from a later
// version of the database, even if the key is known.
static bool enable_secure_delete(sqlite3 *db) {
  return exec_pragma(db, "PRAGMA secure_delete = true", "1");
}

// Enable WAL journaling mode for higher write performance.
// More information: https://www.sqlite.org/wal.html
static bool enable_wal(sqlite3 *db) {
  return
      // Lowest level of synchronicity that we can get away with without losing
      // consistency. (Note that we lose durability, but that's probably okay.)
      exec_sql(db, "PRAGMA synchronous = normal") &&
      // Verify result of previous statement.
      exec_pragma(db, "PRAGMA synchronous", "1" /*normal*/) &&
      // Enable write-ahead logging journaling mode.
      exec_pragma(db, "PRAGMA journal_mode = wal", "wal");
}

// Reset journaling mode to allow the database to be opened in read-only mode
// (assuming it was cleanly closed and doesn't need recovery at start-up).
static bool disable_wal(sqlite3 *db) {
  return
    exec_sql(db, "PRAGMA synchronous = full") &&
    exec_pragma(db, "PRAGMA synchronous", "2" /*full*/) &&
    exec_pragma(db, "PRAGMA journal_mode = delete", "delete");
}

// Sets the encryption parameters (password and cipher page size) of the
// database. This must be done immediately after opening the database.
// If password == NULL, this function does nothing but returns true.
static bool set_password(sqlite3 *db, const char *password, int kdf_iter, int cipher_compatibility) {
  if (password == NULL) {
    return true;
  }
  if (kdf_iter < 0 || cipher_compatibility < MIN_CIPHER_COMPAT) {
    return false;
  }

  if (sqlite3_key(db, password, strlen(password)) != SQLITE_OK) {
    return false;
  }

  // Select parameters from SQLCipher version 3 for backward compatibilty
  // when using SQLCipher version 4.
  //
  // Practically, this has the effect of selecting the following parameters:
  //
  // PRAGMA                    version 3 default   version 4 default
  // ------------------------ ------------------ --------------------
  // cipher_page_size                      1024                 4096
  // kdf_iter                             64000               256000
  // cipher_hmac_algorithm            HMAC_SHA1          HMAC_SHA256
  // cipher_kdf_algorithm      PBKDF2_HMAC_SHA1   PBKDF2_HMAC_SHA512
  //
  // Note that the cipher_compatibility PRAGMA is supported since version 4
  // only. Version 3 will ignore the PRAGMA, but use the correct defaults.
  // Version 2 and below are not supported.
  char buf[64];
  snprintf(buf, sizeof(buf), "PRAGMA cipher_compatibility = %d", cipher_compatibility);
  if (!exec_sql(db, buf)) {
    return false;
  }

  // We support overriding kdf_iter to speed up tests only!
  if (kdf_iter > 0) {
    snprintf(buf, sizeof(buf), "PRAGMA kdf_iter = %d", kdf_iter);
    if (!exec_sql(db, buf)) {
      return false;
    }
  }

  return true;
}

static bool validate_options(const struct sqlfs_options *options) {
  if (options == NULL) {
    return false;
  }
  if (options->filepath == NULL) {
    return false;
  }
  if (options->kdf_iter < 0) {
    return false;
  }
  return true;
}

int sqlfs_create(const struct sqlfs_options *options) {
  if (!validate_options(options)) {
    return EINVAL;
  }

  int err = EIO;
  sqlite3 *db = NULL;

  int open_flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
  if (sqlite3_open_v2(options->filepath, &db, open_flags, NULL) != SQLITE_OK) {
    goto failure;
  }
  struct sqlcipher_version sqlcipher_version;
  if (!get_supported_sqlcipher_version(&sqlcipher_version)) {
    goto failure;
  }
  int cipher_compat = int_min(MAX_CIPHER_COMPAT, sqlcipher_version.major);
  if (!set_password(db, options->password, options->kdf_iter, cipher_compat)) {
    goto failure;
  }
  if (!enable_exclusive(db) || !enable_secure_delete(db) || !enable_wal(db)) {
    goto failure;
  }

  int64_t user_version = get_user_version(db);
  if (user_version != 0) {
    fprintf(stderr, "Wrong schema version: %lld (expected 0)\n", (long long)user_version);
    goto failure;
  }

  if (!exec_sql(db, "BEGIN TRANSACTION")) {
    goto failure;
  }

#define SQL_STATEMENT(sql) if (!exec_sql(db, #sql)) goto failure;
#include "sqlfs_schema.h"
#undef SQL_STATEMENT

#define STR2(s) #s
#define STR(s) STR2(s)
  if (!exec_sql(db, "PRAGMA user_version = " STR(SQLFS_SCHEMA_VERSION))) {
    goto failure;
  }
#undef STR
#undef STR2

  if (!create_root_directory(db, options->umask, options->uid, options->gid)) {
    goto failure;
  }

  if (!exec_sql(db, "COMMIT TRANSACTION")) {
    goto failure;
  }

  if (!disable_wal(db)) {
    goto failure;
  }

  err = 0; // Success.

failure:
  // It's safe to call sqlite3_close on a NULL pointer.
  // If a transaction is open, it will be rolled back.
  if (sqlite3_close(db) != SQLITE_OK) {
    err = EIO;
  }
  return err;
}

struct sqlfs *sqlfs_open(enum sqlfs_open_mode mode,
    const struct sqlfs_options *options) {

  if (mode != SQLFS_OPEN_MODE_READONLY && mode != SQLFS_OPEN_MODE_READWRITE) {
    DLOG("%s() invalid mode=%d\n", __func__, mode);
    return NULL;
  }

  if (!validate_options(options)) {
    DLOG("%s() invalid options\n", __func__);
    return NULL;
  }

  struct sqlfs *sqlfs = calloc(1, sizeof(struct sqlfs));
  CHECK(sqlfs);
  sqlfs->umask = options->umask;
  sqlfs->uid = options->uid;
  sqlfs->gid = options->gid;
  sqlfs->blocksize = 4096;  /* default Linux pagesize */

  struct sqlcipher_version sqlcipher_version;
  if (!get_supported_sqlcipher_version(&sqlcipher_version)) {
    goto failure;
  }
  int open_flags = mode == SQLFS_OPEN_MODE_READONLY ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
  // The database may have been created with older cypher parameters. For
  // backward compatibility, we'll try opening the database with decreasing
  // cipher_compat values. This means opening older databases will be slower,
  // but at least it works.
  int max_cipher_compat = int_min(MAX_CIPHER_COMPAT, sqlcipher_version.major);
  CHECK(max_cipher_compat >= MIN_CIPHER_COMPAT);
  int cipher_compat = max_cipher_compat;
  int64_t user_version = -1;
try_open:
  if (sqlite3_open_v2(options->filepath, &sqlfs->db, open_flags, NULL) != SQLITE_OK) {
    goto failure;
  }
  if (!set_password(sqlfs->db, options->password, options->kdf_iter, cipher_compat)) {
    goto failure;
  }
  // First, get the database schema version. Since this is the first query we
  // do, this is likely to fail if the database is locked or encrypted in an
  // incompatible way, so we'll check the status (instead of simply calling
  // get_user_version()) and print a helpful error message for some of the
  // most common failure reasons.
  int status = get_user_version_with_status(sqlfs->db, &user_version);
  if (status == SQLITE_NOTADB && cipher_compat > MIN_CIPHER_COMPAT) {
    // File format not recognized! This commonly happens because the password
    // is wrong, or the database was created with different cipher parameters.
    // Re-open the database to retry with a lower cipher compatibility level.
    CHECK(sqlite3_close(sqlfs->db) == SQLITE_OK);
    sqlfs->db = NULL;
    --cipher_compat;
    goto try_open;
  }
  if (status != SQLITE_ROW) {
    // Some non-retryable error occurred. Report it.
    fprintf(stderr, "Failed to query database version: %s!\n", sqlite3_errmsg(sqlfs->db));
    if (status == SQLITE_BUSY || status == SQLITE_LOCKED) {
      fprintf(stderr, "Database may be in use by another process.\n");
    }
    if (status == SQLITE_NOTADB) {
      fprintf(stderr, "Database may be encrypted with a different password.\n");
    }
    goto failure;
  }
  if (cipher_compat < max_cipher_compat) {
    fprintf(stderr,
        "NOTICE: Database was opened with SQLCipher version %d compatibility.\n"
        "        The current SQLCipher library has major version %d.\n"
        "        Run `sqlfuse cipher_migrate` to migrate the database.\n",
        cipher_compat, max_cipher_compat);
  }
  if (!enable_exclusive(sqlfs->db) || !enable_secure_delete(sqlfs->db)) {
    goto failure;
  }
  if (mode == SQLFS_OPEN_MODE_READWRITE) {
    if (!enable_wal(sqlfs->db)) {
      fprintf(stderr, "Failed to open database in writable mode: %s!\n", sqlite3_errmsg(sqlfs->db));
      fprintf(stderr, "Possible explanations:\n"
          "  - The database is in use by another process.\n"
          "  - You do not have write permissions on the database file.\n"
          "  - You do not have write permissions on its containing directory.\n");
      goto failure;
    }
    sqlfs->wal_enabled = true;
  }

  if (user_version != SQLFS_SCHEMA_VERSION) {
    fprintf(stderr, "Wrong schema version number: %lld (expected %d)\n", (long long)user_version, SQLFS_SCHEMA_VERSION);
    goto failure;
  }

  // This must be done after the schema has been created.
  for (int i = 0; i < NUM_STATEMENTS; ++i) {
    CHECK(statements[i].id == i);
    if (!prepare(sqlfs->db, statements[i].sql, &sqlfs->stmt[i])) goto failure;
  }

  return sqlfs;

failure:
  sqlfs_close(sqlfs);
  return NULL;
}

int sqlfs_cipher_migrate(const char *filepath, const char *password) {
  if (filepath == NULL || password == NULL) {
    return EINVAL;
  }
  struct sqlcipher_version sqlcipher_version;
  if (sqlfs_get_sqlcipher_version(&sqlcipher_version) != 0) {
    fprintf(stderr, "Failed to determine SQLCipher version.\n");
    return EIO;
  }
  if (sqlcipher_version.major <= MIN_CIPHER_COMPAT) {
    fprintf(stderr, "SQLCipher version (%d.%d%.d) too low (must be greater than %d).\n",
        sqlcipher_version.major, sqlcipher_version.minor, sqlcipher_version.patch, MIN_CIPHER_COMPAT);
    return ENOTSUP;
  }
  if (sqlcipher_version.major > MAX_CIPHER_COMPAT) {
    fprintf(stderr, "SQLCipher version (%d.%d%.d) too high (must be at most %d).\n",
        sqlcipher_version.major, sqlcipher_version.minor, sqlcipher_version.patch, MAX_CIPHER_COMPAT);
    return ENOTSUP;
  }

  bool success = false;
  sqlite3 *db = NULL;
  // Database must be opened with mode SQLITE_OPEN_CREATE (even though the file
  // should already exist) or the migration will fail.
  if (sqlite3_open_v2(filepath, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
    fprintf(stderr, "Failed to open database file in read/write mode.\n");
  } else if (sqlite3_key(db, password, strlen(password)) != SQLITE_OK) {
    fprintf(stderr, "Failed to set password.\n");
  } else {
    fprintf(stderr, "Migrating to SQLCipher version %d. Please wait...\n", sqlcipher_version.major);
    sqlite3_stmt *stmt = NULL;
    CHECK(prepare(db, "PRAGMA cipher_migrate", &stmt));
    int migrate_result = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      migrate_result = sqlite3_column_int(stmt, 0);
      CHECK(sqlite3_step(stmt) == SQLITE_DONE);
    }
    sqlite3_finalize(stmt);
    if (migrate_result != 0) {
      fprintf(stderr, "Migration failed! Was the password correct?\n");
    } else {
      fprintf(stderr, "Migration completed succesfully!\n");
      success = true;
    }
  }
  CHECK(sqlite3_close(db) == SQLITE_OK);
  return success ? 0 : EIO;
}

void sqlfs_close(struct sqlfs *sqlfs) {
  if (sqlfs == NULL) {
    return;
  }
  // From the SQLite docs:
  //  "Applications should finalize all prepared statements, close all BLOB
  //   handles, and finish all sqlite3_backup objects associated with the
  //   sqlite3 object prior to attempting to close the object."
  CHECK(sqlfs->dir_stmt == NULL);
  for (int i = 0; i < NUM_STATEMENTS; ++i) {
    if (sqlfs->stmt[i]) {
      CHECK(sqlite3_finalize(sqlfs->stmt[i]) == SQLITE_OK);
    }
  }

  if (sqlfs->db) {
    // Reset the journaling mode. This allows the database file to be opened in
    // read-only mode later (which is not possible if the database is in WAL
    // mode.)
    if (sqlfs->wal_enabled && !disable_wal(sqlfs->db)) {
      DLOG("%s() Failed to disable WAL!\n", __func__);
    }
    CHECK(sqlite3_close(sqlfs->db) == SQLITE_OK);
  }
  free(sqlfs);
}

int sqlfs_rekey(struct sqlfs *sqlfs, const char *new_password) {
  if (new_password == NULL) {
    return EINVAL;
  }

  // Note: rekeying the database may corrupt the database if any other process
  // has the database file opened. sqlite3_rekey() does NOT check this! We rely
  // on an exclusive lock being obtained in sqlfs_open() to guarantee that no
  // other process is using the database while we rekey.
  int status = sqlite3_rekey(sqlfs->db, new_password, strlen(new_password));
  return status == SQLITE_OK ? 0 : EIO;
}

int sqlfs_vacuum(struct sqlfs *sqlfs) {
  return exec_sql(sqlfs->db, "VACUUM") ? 0 : EIO;
}

int sqlfs_get_blocksize(const struct sqlfs *sqlfs) {
  return sqlfs->blocksize;
}

void sqlfs_set_blocksize(struct sqlfs *sqlfs, int blocksize) {
  CHECK(blocksize > 0 && blocksize < INT32_MAX);
  sqlfs->blocksize = blocksize;
}

int sqlfs_stat(struct sqlfs *sqlfs, ino_t ino, struct stat *stat) {
  sqlite3_stmt *stmt = sqlfs->stmt[STMT_STAT];
  CHECK(sqlite3_bind_int64(stmt, PARAM_STAT_INO, ino) == SQLITE_OK);
  return finish_stat_query(stmt, stat, sqlfs->blocksize);
}

// Like sqlfs_stat(), but also verifies that `ino` refers to a directory, and
// that it has positive link count.
//
// This function is used by sqlfs_mkdir() and sqlfs_mknod(), before calling
// sql_insert_direntries() to insert new entries in a directory. An unlinked
// directory effectively doesn't exist, so it should not be possible to create
// files or subdirectories in it.
//
// Returns:
//  0 on success
//  ENOENT if the entry does not exist, or its link count is zero
//  ENOTDIR if the entry exists, but does not refer to a directory
//  EIO if a database operation failed
static int stat_dir(struct sqlfs *sqlfs, ino_t ino, struct stat *stat) {
  int err = sqlfs_stat(sqlfs, ino, stat);
  if (err != 0) {
    return err;
  }
  if (!S_ISDIR(stat->st_mode)) {
    return ENOTDIR;
  }
  if (stat->st_nlink == 0) {
    return ENOENT;
  }
  return 0;
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

  sql_savepoint(sqlfs);

  struct stat dir_stat;
  int err = stat_dir(sqlfs, dir_ino, &dir_stat);
  if (err != 0) {
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
  if (err != 0) {
    sql_rollback_to_savepoint(sqlfs);
  }
  sql_release_savepoint(sqlfs);
  return err;
}

// Unlinks a file (if dir == false) or a directory (if dir == true).
//
// Used to implement sqlfs_rmdir(), sqlfs_unlink(), and sqlfs_rename().
//
// On success, this function returns 0 and the inode number of the removed entry
// is written to *child_ino_out.
//
// Minor bug: this function should technicaly return ENOTDIR instead of ENOENT if
// dir_ino does not refer to a directory. (Perhaps this isn't an issue in practice,
// since FUSE verifies dir_ino refers to a directory before calling any filesystem
// implementation functions.)
//
// Returns:
//  0 on success
//  ENOENT if the referenced entry does not exist
//  EINVAL if name was empty or '.'
//  EBUSY if the entry referred to the root directory (which cannot be removed)
//  EIO if a database operation failed
//  ENOTDIR if (dir == true) and the entry does not refer to a directory
//  EISDIR if (dir == false) and the entry does not refer to a file
//  ENOTEMPTY if (dir == true) and the entry refers to a directory which is not empty
static int remove_impl(struct sqlfs *sqlfs, ino_t dir_ino, const char *name, bool dir, ino_t *child_ino_out) {
  sql_savepoint(sqlfs);
  int err = -1;

  // Find the entry by its name in the given directory.
  ino_t child_ino = SQLFS_INO_NONE;
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
    if (child_ino == SQLFS_INO_ROOT) {
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
  goto finish;
rollback:
  CHECK(err > 0);
  sql_rollback_to_savepoint(sqlfs);
finish:
  sql_release_savepoint(sqlfs);
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

int sqlfs_dir_next(struct sqlfs *sqlfs, const char **name, ino_t *ino, mode_t *mode) {
  sqlite3_stmt *stmt = sqlfs->dir_stmt;
  CHECK(stmt);
  int status = sqlite3_step(stmt);
  if (status == SQLITE_ROW) {
    const char *n = (const char *) sqlite3_column_text(stmt, COL_READ_DIRENTRIES_ENTRY_NAME);
    CHECK(n);
    *name = *n ? n : "..";
    *ino = sqlite3_column_int64(stmt, COL_READ_DIRENTRIES_ENTRY_INO);
    *mode = sqlite3_column_int64(stmt, COL_READ_DIRENTRIES_ENTRY_TYPE) << 12;
    return 0;
  }
  if (status == SQLITE_DONE) {
    *name = NULL;
    *ino = SQLFS_INO_NONE;
    *mode = 0;
    return 0;
  }
  DLOG("%s() status=%d\n", __func__, status);
  return EIO;
}

void sqlfs_dir_close(struct sqlfs *sqlfs) {
  sqlite3_stmt *stmt = sqlfs->dir_stmt;
  CHECK(stmt);
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  sqlfs->dir_stmt = NULL;
}

int sqlfs_mknod(struct sqlfs *sqlfs, ino_t dir_ino, const char *name, mode_t mode, struct stat *stat) {
  memset(stat, 0, sizeof(*stat));

  if (name_kind(name) != NAME_REGULAR) {
    return EINVAL;
  }
  if (mode != ((mode & 0777) | S_IFREG)) {
    return EINVAL;
  }
  mode = (mode & 0777 & ~sqlfs->umask) | S_IFREG;

  sql_savepoint(sqlfs);

  struct stat dir_stat;
  int err = stat_dir(sqlfs, dir_ino, &dir_stat);
  if (err != 0) {
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
  if (err != 0) {
    sql_rollback_to_savepoint(sqlfs);
  }
  sql_release_savepoint(sqlfs);
  return err;
}

int sqlfs_unlink(struct sqlfs *sqlfs, ino_t dir_ino, const char *name, ino_t *child_ino_out) {
  return remove_impl(sqlfs, dir_ino, name, false /* dir */, child_ino_out);
}

int sqlfs_purge(struct sqlfs *sqlfs, ino_t ino) {
  sql_savepoint(sqlfs);

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
    err = sql_delete_filedata(sqlfs, ino, INT64_MIN);
  }
  if (err != 0) {
    // err is EIO
    goto finish;
  }
  // Finally delete file/directory metadata.
  err = sql_delete_metadata(sqlfs, ino);

finish:
  if (err != 0) {
    sql_rollback_to_savepoint(sqlfs);
  }
  sql_release_savepoint(sqlfs);
  return err;
}

int sqlfs_purge_all(struct sqlfs *sqlfs) {
  // We prepare a one-off SQL statement, because we expect this function to be
  // called only rarely, so it's not worth keeping it in memory.
  sqlite3_stmt *stmt = NULL;
  CHECK(prepare(sqlfs->db, "SELECT ino FROM metadata WHERE nlink = 0", &stmt));
  int err = 0;
  sql_savepoint(sqlfs);
  for (int status; err == 0 && (status = sqlite3_step(stmt)) != SQLITE_DONE; ) {
    if (status != SQLITE_ROW) {
      err = EIO;
    } else {
      err = sqlfs_purge(sqlfs, (ino_t)sqlite3_column_int64(stmt, 0));
    }
  }
  if (err != 0) {
    sql_rollback_to_savepoint(sqlfs);
  }
  sql_release_savepoint(sqlfs);
  sqlite3_finalize(stmt);
  return err;
}

// Changes the size of a file from `old_size` to `new_size`, padding the current
// data with zeroes if necessary.
//
// Only the filedata table is updated. Metadata must be updated separately.
static int resize_filedata(struct sqlfs *sqlfs, ino_t ino, off_t old_size, off_t new_size, off_t blksize) {
  CHECK(old_size >= 0);
  CHECK(new_size >= 0);
  CHECK(blksize > 0);

  if (old_size == new_size) {
    return 0;
  }

  char *block = calloc(blksize, 1);
  if (block == NULL) {
    DLOG("%s(dir_ino=%lld): unable to allocate %lld bytes!\n",
        __func__, (long long)ino, (long long)blksize);
    // Return EIO because there doesn't seem to be a more suitable errno.
    return EIO;
  }

  int err = 0;

  const off_t old_blocks = (old_size + blksize - 1) / blksize;
  const off_t new_blocks = (new_size + blksize - 1) / blksize;
  if (new_size > old_size) {
    // pos = old_size rounded down to a block boundary.
    off_t pos = old_size - old_size % blksize;
    if (pos < old_size) {
      // Read partial data from the last block.
      err = sql_read_filedata(sqlfs, ino, blksize, pos, old_size - pos, block);
      if (err != 0) {
        goto failure;
      }
    }
    while (pos < new_size) {
      // Size of the next block to write: min(blksize, new_size - pos)
      int64_t size = int64_min(new_size - pos, blksize);
      err = sql_update_filedata(sqlfs, ino, pos/blksize, block, size);
      if (err != 0) {
        goto failure;
      }
      if (pos < old_size) {
        // Clear out partial data from first block.
        memset(block, 0, old_size - pos);
      }
      pos += size;
    }
  } else {  // new_size < old_size
    // Truncate.
    if (new_blocks < old_blocks) {
      // Discard excess blocks.
      err = sql_delete_filedata(sqlfs, ino, new_blocks);
      if (err != 0) {
        goto failure;
      }
    }
    // pos = new_size rounded down to a block boundary.
    off_t pos = new_size - new_size % blksize;
    if (pos < new_size) {
      // Truncate last block, keeping the old bytes.
      err = sql_read_filedata(sqlfs, ino, blksize, pos, new_size - pos, block);
      if (err != 0) {
        goto failure;
      }
      err = sql_update_filedata(sqlfs, ino, pos/blksize, block, new_size - pos);
      if (err != 0) {
        goto failure;
      }
    }
  }
  CHECK(err == 0);
  goto cleanup;

failure:
  CHECK(err != 0);
  goto cleanup;

cleanup:
  free(block);
  return err;
}

int sqlfs_set_attr(struct sqlfs *sqlfs, ino_t ino, const struct stat *attr_in, unsigned to_set, struct stat *attr_out) {
  CHECK(attr_in != attr_out);
  memset(attr_out, 0, sizeof(*attr_out));

  if (to_set != (to_set & SQLFS_SET_ATTR_ALL)) {
    // Unknown flags passed in `to_set`.
    DLOG("%s() to_set=%u\n", __func__, to_set);
    return EINVAL;
  }

  sql_savepoint(sqlfs);

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
    err = resize_filedata(sqlfs, ino, attr_out->st_size, attr_in->st_size, attr_out->st_blksize);
    if (err != 0) {
      goto failure;
    }
    attr_out->st_size = attr_in->st_size;
  }

  if (to_set != 0) {
    err = sql_update_metadata(sqlfs, attr_out);
    if (err != 0) {
      goto failure;
    }
  }

  CHECK(err == 0);
  goto finish;
failure:
  CHECK(err != 0);
  sql_rollback_to_savepoint(sqlfs);
finish:
  sql_release_savepoint(sqlfs);
  return err;
}

int sqlfs_read(struct sqlfs *sqlfs, ino_t ino, off_t off, size_t size, char *buf, size_t *size_read) {
  CHECK(off >= 0 && off < INT64_MAX);
  CHECK(size < INT64_MAX);

  sql_savepoint(sqlfs);

  struct stat attr = {0};
  int err = sqlfs_stat(sqlfs, ino, &attr);
  if (err != 0) {
    goto failure;
  }
  if (S_ISDIR(attr.st_mode)) {
    err = EISDIR;
    goto failure;
  }
  if (!S_ISREG(attr.st_mode)) {
    err = EINVAL;
    goto failure;
  }

  int64_t size_to_read = int64_max(0, int64_min(size, (int64_t)attr.st_size - (int64_t)off));
  if (size > 0) {
    err = sql_read_filedata(sqlfs, ino, attr.st_blksize, off, size_to_read, buf);
    if (err != 0) {
      goto failure;
    }
  }
  *size_read = size_to_read;

  CHECK(err == 0);
  goto finish;
failure:
  CHECK(err != 0);
  sql_rollback_to_savepoint(sqlfs);
finish:
  sql_release_savepoint(sqlfs);
  return err;
}

int sqlfs_write(struct sqlfs *sqlfs, ino_t ino, off_t off, size_t size, const char *buf) {
  CHECK(off >= 0 && off < INT64_MAX);
  CHECK(size < INT64_MAX);

  char *temp_block = NULL;

  sql_savepoint(sqlfs);

  struct stat attr = {0};
  int err = sqlfs_stat(sqlfs, ino, &attr);
  if (err != 0) {
    goto failure;
  }
  if (S_ISDIR(attr.st_mode)) {
    err = EISDIR;
    goto failure;
  }
  if (!S_ISREG(attr.st_mode)) {
    err = EINVAL;
    goto failure;
  }

  const int64_t blksize = attr.st_blksize;
  const int64_t new_size = int64_max(off + size, attr.st_size);

  if (off > attr.st_size) {
    // Write is past-the-end of the file. Extend the file to `off`, filling the
    // gap with zeroes.
    err = resize_filedata(sqlfs, ino, attr.st_size, off, blksize);
    if (err != 0) {
      goto failure;
    }
    attr.st_size = off;
  }

  for (int64_t i = off / blksize; i * blksize < off + (int64_t)size; ++i) {
    const int64_t chunk_offset = i * blksize;

    // Size of the chunk to be written. How far this extends depends not only
    // on the range to be written, but also on the current size of the file,
    // since we don't want to truncate the file if the write ends before the
    // end of the block.
    const int64_t chunk_size = int64_min(new_size - chunk_offset, blksize);
    CHECK(chunk_size > 0);

    // buf_offset is the position in `buf` from which to start copying.
    const int64_t buf_offset = (int64_t)chunk_offset - (int64_t)off;

    // Pointer to the chunk data to write. This will either point into `buf`
    // directly, or into a temporary buffer assigned to `temp_block`, depending on
    // whether the chunk to be written consists entirely of data from `buf` or
    // has to be combined with a prefix and/or suffix of old data read from the
    // filedata table.
    const char *chunk_ptr = NULL;

    if (chunk_offset >= off && chunk_offset + chunk_size <= off + (int64_t)size) {
      // Easy case: chunk to write will contain data from `buf` only.
      chunk_ptr = buf + buf_offset;
    } else {
      // Hard case: the block to write contains a mix of old and new data. We'll
      // allocate a temporary block, fill it with old data, then copy over the
      // new data that falls in range.

      // Allocate a temporary buffer (if we don't have one yet). We might use it
      // at most two times: once for the first temp_block, and once for the last.
      if (temp_block == NULL) {
        // It's enough to allocate `chunk_size` because if we need more than one
        // temp_block, the first one will be the full `blksize` in length.
        temp_block = malloc(chunk_size);
        if (temp_block == NULL) {
          DLOG("%s(dir_ino=%lld): unable to allocate %lld bytes!\n",
              __func__, (long long)ino, (long long)blksize);
          // Return EIO because there doesn't seem to be a more suitable errno.
          err = EIO;
          goto failure;
        }
      }

      // Defensively fill temp_block with -1s to detect any bugs. (If everything
      // is right, the contents should be completely overwritten below, but the
      // logic is complicated.)
      memset(temp_block, -1, chunk_size);

      // Read old data into `temp_block`. This may be less than `chunk_size`,
      // because the range to be written may extend beyond the end of the file.
      int64_t bytes_to_read = int64_min(chunk_size, attr.st_size - chunk_offset);
      CHECK(bytes_to_read > 0);
      err = sql_read_filedata(sqlfs, ino, blksize, chunk_offset, bytes_to_read, temp_block);
      if (err != 0) {
        goto failure;
      }

      // Copy the new data into `temp_block`.
      if (buf_offset < 0) {
        int64_t bytes_to_copy = int64_min(blksize + buf_offset, size);
        CHECK(bytes_to_copy > 0 && bytes_to_copy <= chunk_size);
        memcpy(temp_block - buf_offset, buf, bytes_to_copy);
      } else {  // buf_offset >= 0
        int64_t bytes_to_copy = int64_min(blksize, size - buf_offset);
        CHECK(bytes_to_copy > 0 && bytes_to_copy <= chunk_size);
        memcpy(temp_block, buf + buf_offset, bytes_to_copy);
      }

      chunk_ptr = temp_block;
    }

    // Actually write the next chunk.
    err = sql_update_filedata(sqlfs, ino, i, chunk_ptr, chunk_size);
    if (err != 0) {
      goto failure;
    }
  }

  // Update metadata: size and mtime changed.
  CHECK(err == 0);
  attr.st_size = new_size;
  attr.st_mtim = attr.st_ctim = attr.st_atim = current_timespec();
  err = sql_update_metadata(sqlfs, &attr);
  if (err != 0) {
    goto failure;
  }

  CHECK(err == 0);
  goto finish;
failure:
  CHECK(err != 0);
  sql_rollback_to_savepoint(sqlfs);
finish:
  sql_release_savepoint(sqlfs);
  free(temp_block);
  return err;
}

int sqlfs_rename(struct sqlfs *sqlfs, ino_t old_parent_ino, const char *old_name, ino_t new_parent_ino, const char *new_name, ino_t *old_ino) {

  *old_ino = SQLFS_INO_NONE;

  if (name_kind(old_name) != NAME_REGULAR ||
      name_kind(new_name) != NAME_REGULAR) {
    return EINVAL;
  }

  int err = -1;
  sql_savepoint(sqlfs);

  struct stat old_parent_attr;
  struct stat new_parent_attr;
  struct stat old_entry_attr;
  struct stat new_entry_attr;

  err = sqlfs_stat(sqlfs, old_parent_ino, &old_parent_attr);
  if (err != 0) {
    goto failure;
  }
  if (!S_ISDIR(old_parent_attr.st_mode)) {
    err = ENOTDIR;
    goto failure;
  }

  err = sqlfs_stat(sqlfs, new_parent_ino, &new_parent_attr);
  if (err != 0) {
    goto failure;
  }
  if (!S_ISDIR(new_parent_attr.st_mode)) {
    err = ENOTDIR;
    goto failure;
  }

  err = sqlfs_stat_entry(sqlfs, old_parent_ino, old_name, &old_entry_attr);
  if (err != 0) {
    goto failure;
  }
  const bool is_dir = S_ISDIR(old_entry_attr.st_mode);

  err = sqlfs_stat_entry(sqlfs, new_parent_ino, new_name, &new_entry_attr);
  if (err == ENOENT) {
    // New entry does not exist. This is normal.
    err = 0;
  } else if (err != 0) {
    // Some other error. Abort!
    goto failure;
  } else if (old_entry_attr.st_ino == new_entry_attr.st_ino) {
    // From man rename(2):
    //
    //   If oldpath and newpath are existing hard links referring to the same
    //   file, then rename() does nothing, and returns a success status.
    //
    // (This doesn't make a lot of sense to me, but it does solve the edge case
    // where the old and the new file are the same.)
    err = 0;
    goto success;
  } else {
    // New entry exists. Try to remove it first.
    err = remove_impl(sqlfs, new_parent_ino, new_name, is_dir, old_ino);
    if (err != 0) {
      goto failure;
    }
  }

  // Now the new entry doesn't exist. Rename the old to the new entry.
  err = sql_rename(sqlfs, old_parent_ino, old_name, new_parent_ino, new_name);
  if (err != 0) {
    goto failure;
  }
  if (is_dir && old_parent_ino != new_parent_ino) {
    // If we moved a child directory to a different subdirectory, we must update
    // the child directory's parent reference, and adjust the old and new parent's
    // link count accordingly.
    err = sql_reparent_directory(sqlfs, old_entry_attr.st_ino, new_parent_ino);
    if (err != 0) {
      goto failure;
    }
    err = sql_dec_nlink(sqlfs, old_parent_ino);
    if (err != 0) {
      goto failure;
    }
    err = sql_inc_nlink(sqlfs, new_parent_ino);
    if (err != 0) {
      goto failure;
    }
  }

success:
  CHECK(err == 0);
  goto finish;
failure:
  CHECK(err != 0);
  sql_rollback_to_savepoint(sqlfs);
finish:
  sql_release_savepoint(sqlfs);
  return err;
}

int sqlfs_sync(struct sqlfs *sqlfs) {
  // We want any pending changes to be synced to disk permanently. From the
  // SQLite documentation: "[SQLITE_CHECKPOINT_FULL] checkpoints all frames in
  // the log file and syncs the database file".
  int status = sqlite3_wal_checkpoint_v2(sqlfs->db, NULL, SQLITE_CHECKPOINT_FULL, NULL, NULL);
  return status == SQLITE_OK ? 0 : EIO;
}
