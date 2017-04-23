#include "sqlfs.h"

#include <assert.h>
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
  STMT_STAT,
  NUM_STATEMENTS };


static const char *statements[NUM_STATEMENTS] = {
  /* STMT_BEGIN_TRANSACTION */
  "BEGIN TRANSACTION",

  /* STMT_COMMIT_TRANSACTION */
  "COMMIT TRANSACTION",

  /* STMT_STAT */
#define PARAM_STAT_INO   1
#define COL_STAT_MODE    0
#define COL_STAT_NLINK   1
#define COL_STAT_UID     2
#define COL_STAT_GID     3
#define COL_STAT_SIZE    4
#define COL_STAT_BLKSIZE 5
#define COL_STAT_MTIME   6
  "SELECT mode, nlink, uid, gid, size, blksize, mtime FROM metadata WHERE ino = ?"
};

struct sqlfs {
  sqlite3 *db;
  mode_t umask;
  uid_t uid;
  gid_t gid;
  sqlite3_stmt *stmt[NUM_STATEMENTS];
};

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

static int64_t current_time_nanos() {
  struct timespec tp;
  CHECK(clock_gettime(CLOCK_REALTIME, &tp) == 0);
  return (int64_t)tp.tv_sec * NANOS_PER_SECOND + tp.tv_nsec;
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

static void create_root_directory(struct sqlfs *sqlfs) {
  sqlite3_stmt *stmt = NULL;

  const mode_t mode = (0777 &~ sqlfs->umask) | S_IFDIR;
  CHECK(prepare(sqlfs->db, "INSERT INTO metadata(ino, mode, nlink, uid, gid, mtime) VALUES (?, ?, ?, ?, ?, ?)", &stmt));
  CHECK(sqlite3_bind_int64(stmt, 1, ROOT_INO) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, 2, mode) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, 3, 2) == SQLITE_OK);  // nlink
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

  for (int i = 0; i < NUM_STATEMENTS; ++i) {
    if (!prepare(sqlfs->db, statements[i], &sqlfs->stmt[i])) goto failed;
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
  assert(sqlfs);
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
  sqlite3_bind_int64(stmt, PARAM_STAT_INO, ino);
  int status = sqlite3_step(stmt);
  if (status != SQLITE_ROW) goto finished;
  memset(stat, 0, sizeof(struct stat));
  stat->st_ino = ino;
  stat->st_mode = sqlite3_column_int64(stmt, COL_STAT_MODE);
  stat->st_nlink = sqlite3_column_int64(stmt, COL_STAT_NLINK);
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
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
finished:
  CHECK(sqlite3_clear_bindings(stmt) == SQLITE_OK);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  if (status == SQLITE_ROW) {
    return 0;  // Success.
  }
  if (status == SQLITE_DONE) {
    return ENOENT;  // Row not found.
  }
  return EIO;  // Other SQLite error.
}
