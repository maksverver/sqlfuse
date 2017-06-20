#include "intmap.h"

#include <stdlib.h>

#include <sqlite3.h>

#include "logging.h"

enum statements {
  STMT_SELECT,
  STMT_REPLACE,
  STMT_DELETE,
  STMT_COUNT,
  STMT_RETRIEVE_ONE,
  NUM_STATEMENTS };

static const char *const sql_create =
    "CREATE TABLE intmap(key INTEGER PRIMARY KEY NOT NULL, value NOT NULL)";

static const struct statement {
  int id;
  const char *sql;
} statements[NUM_STATEMENTS] = {
  { STMT_SELECT,
    "SELECT value FROM intmap WHERE key = ?" },
  { STMT_REPLACE,
    "INSERT OR REPLACE INTO intmap(key, value) VALUES (?, ?)" },
  { STMT_DELETE,
    "DELETE FROM intmap WHERE key = ?" },
  { STMT_COUNT,
    "SELECT COUNT(*) FROM intmap" },
  { STMT_RETRIEVE_ONE,
    "SELECT key, value FROM intmap LIMIT 1" },
};

struct intmap {
  sqlite3 *db;
  sqlite3_stmt *stmt[NUM_STATEMENTS];
};

static int64_t get(struct intmap *intmap, int64_t key) {
  sqlite3_stmt *stmt = intmap->stmt[STMT_SELECT];
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
  sqlite3_stmt *stmt = intmap->stmt[STMT_REPLACE];
  CHECK(sqlite3_bind_int64(stmt, 1, key) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, 2, value) == SQLITE_OK);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
}

static void del(struct intmap *intmap, int64_t key) {
  sqlite3_stmt *stmt = intmap->stmt[STMT_DELETE];
  CHECK(sqlite3_bind_int64(stmt, 1, key) == SQLITE_OK);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
}

struct intmap *intmap_create() {
  struct intmap *intmap = calloc(1, sizeof(struct intmap));
  CHECK(sqlite3_open(":memory:", &intmap->db) == SQLITE_OK);
  CHECK(sqlite3_exec(intmap->db, sql_create, NULL, NULL, NULL) == SQLITE_OK);
  for (int i = 0; i < NUM_STATEMENTS; ++i) {
    CHECK(statements[i].id == i);
    CHECK(sqlite3_prepare_v2(intmap->db, statements[i].sql, -1, &intmap->stmt[i], NULL) == SQLITE_OK);
  }
  return intmap;
}

void intmap_destroy(struct intmap *intmap) {
  CHECK(intmap);
  for (int i = 0; i < NUM_STATEMENTS; ++i) {
    CHECK(sqlite3_finalize(intmap->stmt[i]) == SQLITE_OK);
  }
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
  sqlite3_stmt *stmt = intmap->stmt[STMT_COUNT];
  CHECK(sqlite3_step(stmt) == SQLITE_ROW);
  int64_t size = sqlite3_column_int64(stmt, 0);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  CHECK(sqlite3_reset(stmt) == SQLITE_OK);
  return (size_t)size;
}

bool intmap_retrieve_one(struct intmap *intmap, int64_t *key, int64_t *value) {
  sqlite3_stmt *stmt = intmap->stmt[STMT_RETRIEVE_ONE];
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
