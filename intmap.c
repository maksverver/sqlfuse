#include "intmap.h"

#include <stdlib.h>

#include <sqlite3.h>

#include "logging.h"

static const char *const sql_create =
    "CREATE TABLE intmap(key INTEGER PRIMARY KEY NOT NULL, value NOT NULL)";

static const char *const sql_select =
    "SELECT value FROM intmap WHERE key = ?";

static const char *const sql_replace =
    "INSERT OR REPLACE INTO intmap(key, value) VALUES (?, ?)";

static const char *const sql_delete =
    "DELETE FROM intmap WHERE key = ?";

static const char *const sql_count =
    "SELECT COUNT(*) FROM intmap";

static const char *const sql_retrieve_one =
    "SELECT key, value FROM intmap LIMIT 1";

struct intmap {
  sqlite3 *db;
  sqlite3_stmt *stmt_select;
  sqlite3_stmt *stmt_replace;
  sqlite3_stmt *stmt_delete;
  sqlite3_stmt *stmt_count;
  sqlite3_stmt *stmt_retrieve_one;
};

static int64_t get(struct intmap *intmap, int64_t key) {
  sqlite3_stmt *stmt = intmap->stmt_select;
  CHECK(sqlite3_bind_int64(stmt, 1, key) == SQLITE_OK);
  int status = sqlite3_step(stmt);
  int64_t value;
  if (status == SQLITE_ROW) {
    value = sqlite3_column_int64(stmt, 0);
  } else {
    CHECK(status == SQLITE_DONE);
    value = 0;
  }
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  return value;
}

static void put(struct intmap *intmap, int64_t key, int64_t value) {
  sqlite3_stmt *stmt = intmap->stmt_replace;
  CHECK(sqlite3_bind_int64(stmt, 1, key) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, 2, value) == SQLITE_OK);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
}

static void del(struct intmap *intmap, int64_t key) {
  sqlite3_stmt *stmt = intmap->stmt_delete;
  CHECK(sqlite3_bind_int64(stmt, 1, key) == SQLITE_OK);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
}

struct intmap *intmap_create() {
  struct intmap *intmap = calloc(1, sizeof(struct intmap));
  CHECK(sqlite3_open(":memory:", &intmap->db) == SQLITE_OK);
  CHECK(sqlite3_exec(intmap->db, sql_create, NULL, NULL, NULL) == SQLITE_OK);
  CHECK(sqlite3_prepare_v2(intmap->db, sql_select, -1, &intmap->stmt_select, NULL) == SQLITE_OK);
  CHECK(sqlite3_prepare_v2(intmap->db, sql_replace, -1, &intmap->stmt_replace, NULL) == SQLITE_OK);
  CHECK(sqlite3_prepare_v2(intmap->db, sql_delete, -1, &intmap->stmt_delete, NULL) == SQLITE_OK);
  CHECK(sqlite3_prepare_v2(intmap->db, sql_count, -1, &intmap->stmt_count, NULL) == SQLITE_OK);
  CHECK(sqlite3_prepare_v2(intmap->db, sql_retrieve_one, -1, &intmap->stmt_retrieve_one, NULL) == SQLITE_OK);
  return intmap;
}

void intmap_destroy(struct intmap *intmap) {
  CHECK(intmap);
  CHECK(sqlite3_finalize(intmap->stmt_select) == SQLITE_OK);
  CHECK(sqlite3_finalize(intmap->stmt_replace) == SQLITE_OK);
  CHECK(sqlite3_finalize(intmap->stmt_delete) == SQLITE_OK);
  CHECK(sqlite3_finalize(intmap->stmt_count) == SQLITE_OK);
  CHECK(sqlite3_finalize(intmap->stmt_retrieve_one) == SQLITE_OK);
  CHECK(sqlite3_close(intmap->db) == SQLITE_OK);
  free(intmap);
}

int64_t intmap_update(struct intmap *intmap, int64_t key, int64_t add) {
  int64_t value = get(intmap, key);
  if (add != 0) {
    if (add > 0) {
      CHECK(value <= 0 || add <= INT64_MAX - value);
    } else {  // add < 0
      CHECK(value >= 0 || add >= INT64_MIN - value);
    }
    value += add;
    if (value != 0) {
      put(intmap, key, value);
    } else {
      del(intmap, key);
    }
  }
  return value;
}

size_t intmap_size(struct intmap *intmap) {
  sqlite3_stmt *stmt = intmap->stmt_count;
  CHECK(sqlite3_step(stmt) == SQLITE_ROW);
  int64_t size = sqlite3_column_int64(stmt, 0);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  return (size_t)size;
}

bool intmap_retrieve_one(struct intmap *intmap, int64_t *key, int64_t *value) {
  sqlite3_stmt *stmt = intmap->stmt_retrieve_one;
  bool result = false;
  int status = sqlite3_step(stmt);
  if (status == SQLITE_ROW) {
    *key = sqlite3_column_int64(stmt, 0);
    *value = sqlite3_column_int64(stmt, 1);
    result = true;
    status = sqlite3_step(stmt);
  }
  CHECK(status == SQLITE_DONE);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  return result;
}
