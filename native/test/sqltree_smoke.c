/*
** sqltree_smoke.c -- drive the sqt_* C ABI from plain C.
**
** Loads a schema, parses a batch of statements against it, and prints
** the resulting EDN. Exits non-zero if any invariant we assert on
** fails. This is the first gate before the JNI/Panama/Clojure layers.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *sqt_version(void);
void *sqt_open(const char*);
void sqt_close(void*);
char *sqt_parse(void*, const char*);
char *sqt_load_schema(void*, const char*);
void sqt_free(char*);

static int nfail = 0;

static void expect(const char *haystack, const char *needle, const char *what){
  if( strstr(haystack, needle)==0 ){
    fprintf(stderr, "FAIL: %s\n  missing: %s\n", what, needle);
    nfail++;
  }
}

static const char *zSchema =
  "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL,"
  " email TEXT UNIQUE, score REAL DEFAULT 0.0);"
  "CREATE TABLE orders (oid INTEGER PRIMARY KEY REFERENCES users(id),"
  " uid INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
  " total NUMERIC, created_at TEXT DEFAULT (datetime('now')),"
  " CHECK (total >= 0));"
  "CREATE INDEX idx_orders_uid ON orders(uid DESC) WHERE uid > 10;"
  "CREATE VIEW v_tot AS SELECT uid, sum(total) AS s FROM orders GROUP BY uid;"
  "CREATE TRIGGER t_au AFTER UPDATE OF name ON users BEGIN"
  " UPDATE users SET score = score + 1 WHERE id = old.id; END;";

int main(void){
  void *h;
  char *z;
  char *zErr;

  h = sqt_open(":memory:");
  if( h==0 ){ fprintf(stderr, "FAIL: open\n"); return 1; }
  printf("sqlite %s\n", sqt_version());

  zErr = sqt_load_schema(h, zSchema);
  if( zErr ){ fprintf(stderr, "FAIL: load_schema: %s\n", zErr); return 1; }

  /* 1. resolved select over two joined tables */
  z = sqt_parse(h,
    "SELECT u.name AS who, count(o.oid) AS n"
    " FROM users u LEFT JOIN orders o ON o.uid = u.id"
    " WHERE u.score > ?1 GROUP BY u.name HAVING count(*) > 2"
    " ORDER BY n DESC NULLS LAST LIMIT 5 OFFSET 2;");
  if( z==0 ){ fprintf(stderr, "FAIL: parse join returned NULL\n"); return 1; }
  expect(z, ":node :select", "select node");
  expect(z, ":node :column", "resolved column");
  expect(z, ":name \"name\"", "column name resolved");
  expect(z, ":table \"users\"", "column table resolved");
  expect(z, ":node :binop", "binop present");
  expect(z, ":nulls :last", "nulls last");
  expect(z, ":node :parameter", "parameter");
  printf("%s\n----\n", z);
  sqt_free(z);

  /* 2. star expansion */
  z = sqt_parse(h, "SELECT * FROM users;");
  expect(z, ":name \"email\"", "star expanded incl. email column");
  sqt_free(z);

  /* 3. view expansion in FROM */
  z = sqt_parse(h, "SELECT s FROM v_tot WHERE uid = 1;");
  expect(z, ":view true", "view recognized");
  expect(z, ":node :subquery", "view as subquery");
  sqt_free(z);

  /* 4. CTE + compound + window */
  z = sqt_parse(h,
    "WITH c(x) AS (SELECT 1 UNION SELECT 2)"
    " SELECT row_number() OVER (PARTITION BY x ORDER BY x ROWS BETWEEN"
    " 1 PRECEDING AND CURRENT ROW) FROM c;");
  expect(z, ":node :cte", "cte");
  expect(z, ":op :union", "compound union");
  expect(z, ":node :window", "window");
  expect(z, ":frame :rows", "rows frame");
  sqt_free(z);

  /* 5. VALUES form */
  z = sqt_parse(h, "VALUES (1,'a'), (2,'b');");
  expect(z, ":node :values", "values node");
  sqt_free(z);

  /* 6. DML with resolution */
  z = sqt_parse(h,
    "INSERT INTO orders (uid, total) VALUES (1, 9.5)"
    " ON CONFLICT (uid) WHERE uid > 5 DO UPDATE SET total = total + 1"
    " RETURNING oid, total;");
  expect(z, ":node :insert", "insert node");
  expect(z, ":idx 1", "column idx resolved");
  expect(z, ":node :upsert", "upsert");
  expect(z, ":returning", "returning");
  sqt_free(z);

  z = sqt_parse(h, "UPDATE users SET score = score + ? WHERE name LIKE 'a%';");
  expect(z, ":node :update", "update node");
  sqt_free(z);

  z = sqt_parse(h, "DELETE FROM orders WHERE uid IN (SELECT id FROM users);");
  expect(z, ":node :delete", "delete node");
  expect(z, ":rhs :select", "IN subselect");
  sqt_free(z);

  /* 7. DDL dump against resolved schema */
  z = sqt_parse(h, "CREATE TABLE t2 (a INT STRICT, b TEXT COLLATE NOCASE);");
  expect(z, ":node :table", "create table");
  expect(z, ":affinity :integer", "affinity");
  expect(z, ":collation \"NOCASE\"", "collation");
  sqt_free(z);

  /* 8. error reporting */
  z = sqt_parse(h, "SELECT nosuch FROM users;");
  expect(z, ":node :error", "error node");
  expect(z, "no such column", "error message");
  sqt_free(z);

  /* 9. misc verbs */
  z = sqt_parse(h, "BEGIN; COMMIT; PRAGMA main.foreign_keys = ON; VACUUM INTO 'x.db';");
  expect(z, ":node :begin", "begin");
  expect(z, ":node :commit", "commit");
  expect(z, ":node :pragma", "pragma");
  expect(z, ":node :vacuum", "vacuum");
  sqt_free(z);

  /* 10. multi-statement envelope sql spans */
  z = sqt_parse(h, "SELECT 1; SELECT 2;");
  expect(z, ":sql \"SELECT 1\"", "first span");
  expect(z, ":sql \"SELECT 2\"", "second span");
  sqt_free(z);

  sqt_close(h);
  if( nfail ){
    fprintf(stderr, "%d smoke checks failed\n", nfail);
    return 1;
  }
  printf("smoke OK\n");
  return 0;
}
