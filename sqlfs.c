#include "sqlfs.h"

#include <assert.h>
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

enum statements {
  STMT_BEGIN_TRANSACTION,
  STMT_COMMIT_TRANSACTION,
  NUM_STATEMENTS };

static const char *statements[NUM_STATEMENTS] = {
  /* STMT_BEGIN_TRANSACTION */
  "BEGIN TRANSACTION",
  /* STMT_COMMIT_TRANSACTION */
  "COMMIT TRANSACTION",
};

struct sqlfs {
  sqlite3 *db;
  sqlite3_stmt *stmt[NUM_STATEMENTS];
  mode_t umask;
  uid_t uid;
  gid_t gid;
};

static bool prepare(sqlite3 *db, const char *sql, sqlite3_stmt **stmt) {
  if (sqlite3_prepare_v2(db, sql, -1, stmt, NULL) != SQLITE_OK) {
    fprintf(stderr, "Failed to prepare statement [%s]: %s\n", sql, sqlite3_errmsg(db));
    return false;
  }
  return true;
}

static void exec_stmt(sqlite3_stmt *stmt) {
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
}

static void exec_sql(sqlite3 *db, const char *sql) {
  sqlite3_stmt *stmt = NULL;
  CHECK(prepare(db, sql, &stmt));
  exec_stmt(stmt);
  sqlite3_finalize(stmt);
}

static void sql_begin_transaction(struct sqlfs *sqlfs) {
  exec_stmt(sqlfs->stmt[STMT_BEGIN_TRANSACTION]);
}

static void sql_commit_transaction(struct sqlfs *sqlfs) {
  exec_stmt(sqlfs->stmt[STMT_COMMIT_TRANSACTION]);
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
  return (int64_t)tp.tv_sec * 1000000000 + tp.tv_nsec;
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

  for (int i = 0; i < NUM_STATEMENTS; ++i) {
    if (!prepare(sqlfs->db, statements[i], &sqlfs->stmt[i])) goto failed;
  }

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
    sql_begin_transaction(sqlfs);
#define SQL_STATEMENT(sql) exec_sql(sqlfs->db, #sql);
#include "sqlfs_schema.h"
#undef SQL_STATEMENT
    exec_sql(sqlfs->db, "PRAGMA user_version = 1");
    create_root_directory(sqlfs);
    sql_commit_transaction(sqlfs);
  } else if (version != 1) {
    fprintf(stderr, "Wrong version number: %d (expected 1)\n", (int)version);
    goto failed;
  }

  return sqlfs;

failed:
  sqlfs_destroy(sqlfs);
  return NULL;
}

void sqlfs_destroy(struct sqlfs *sqlfs) {
  /* TODO: wait for threads to finish?  Or should caller do that? */
  /* TODO: Applications should finalize all prepared statements, close all BLOB handles, and finish all sqlite3_backup objects associated with the sqlite3 object prior to attempting to close the object. */
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
