# parseq

Unambiguous, fully-resolved syntax trees for SQLite SQL — built on
SQLite's own parser, exposed to Clojure through both JNI and Panama
(FFM) bindings, with a JDBC bridge that resolves against a live
database's schema.

```clojure
(require '[parseq.core :as pq])

(def p (pq/parser))
(pq/load-schema p "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);")

(pq/parse p "SELECT u.name FROM users u WHERE u.id > ?1")
```

```clojure
{:sqlite "3.54.1",
 :statements
 [{:stmt {:node :select,
          :columns [{:expr {:tk "TK_COLUMN", :node :column,
                            :schema "main", :table "users", :name "name",
                            :idx 1, :affinity :text, :cursor 0},
                     :span "u.name"}],
          :from [{:node :table, :name "users", :alias "u", :cursor 0,
                  :schema "main",
                  :columns [{:name "id", :idx 0, :type "INTEGER",
                             :affinity :integer, :pk true}
                            {:name "name", :idx 1, :type "TEXT",
                             :affinity :text}]}],
          :where {:tk "TK_GT", :node :binop, :op :>,
                  :lhs {:node :column, :table "users", :name "id",
                        :rowid true, :idx -1, :cursor 0},
                  :rhs {:node :parameter, :index 1, :name "?1"}}},
   :sql "SELECT u.name FROM users u WHERE u.id > ?1"}]}
```

Every column reference is pinned to its table, schema, cursor number
and column index, with affinity and constraints carried along. `*` and
views are expanded exactly the way SQLite resolves them. Parse errors
come back as data with byte offsets. Nothing you parse is ever
executed.

## How it works

```
            ┌────────────────────────────────────────────────┐
            │  native/  — one shared library, libsqltree.so  │
            │                                                │
  SQL ────► │  vendored SQLite 3.54.1 amalgamation           │
            │   + native/patch/parsecov.patch (29 hunks)     │
            │   + native/src/sqltree.c (serializer)          │
            │                                                │
            │  hooks fire between name resolution and        │
            │  code generation; trees serialize to EDN       │
            └───────▲───────────────────────────▲────────────┘
                    │ sqt_* C ABI               │ JNI symbols
            ┌───────┴────────┐          ┌───────┴────────┐
            │ parseq.native  │          │ parseq.native  │
            │   .panama      │          │   .jni         │
            │ (java.lang.    │          │ (parseq.sqltree│
            │  foreign)      │          │  .Sqltree)     │
            └───────▲────────┘          └───────▲────────┘
                    └───────────┬───────────────┘
                        parseq.core (Parser, EDN in/out)
                                │
                        parseq.jdbc (schema replay from a JDBC Connection)
```

- **The parser and resolver are SQLite's own.** `native/vendor/fetch.sh`
  downloads the pinned amalgamation (SHA3-256 verified); CMake copies
  it and applies `parsecov.patch`, a reviewable 29-hunk diff that
  inserts guarded hook calls at the boundary where a statement is
  fully name-resolved but code generation has not started. Outside
  `parse` the library behaves exactly like stock SQLite.
- **The dump is post-resolution, pre-optimizer.** For SELECT that is
  right after `sqlite3SelectPrep` — views expanded, `*` expanded,
  names resolved — and *before* subquery flattening or push-down, so
  you get the tree SQLite actually resolved, not a planner rewrite.
  DML resolves the same way codegen would (conflict targets, RETURNING,
  trigger NEW/OLD rows), since SQLite interleaves those.
- **Nothing is ambiguous.** Every expression carries `:tk`, the raw
  SQLite parser token code (`"TK_PLUS"`). Unknown ops fall through to a
  generic node with children instead of being dropped. Verbs without a
  deep dump (ATTACH, ANALYZE, ...) become explicit leaf nodes with the
  raw statement text. A statement that fails to parse appears as
  `{:node :error :message "..." :offset 42}` — `parse` never throws for
  bad SQL.

## Building and running

Requirements: JDK 21+ (FFM bindings are final in 22+), CMake ≥ 3.20,
Ninja, a C compiler, `patch`, `curl` + `openssl` (for the vendored
fetch), Clojure CLI.

```sh
clojure -T:build native   # fetch amalgamation, apply patch, build libsqltree
clojure -T:build javac    # compile the JNI glue class
clojure -T:build test     # both of the above, then the full suite
```

`clojure -M:dev` gives you a REPL with `java.library.path` and
`--enable-native-access` set. The native library is found via
`-Dparseq.lib=/path/to/libsqltree.so`, then `java.library.path`.

## The API

### parseq.core

| form | meaning |
|---|---|
| `(parser {:impl :panama \| :jni :path "..."})` | new session (defaults: Panama, `:memory:`) |
| `(load-schema p ddl)` | execute DDL so later parses resolve against it; returns `{:error "..."}` on rejection |
| `(parse p sql)` | parse one or many statements; returns the tree; never throws for bad SQL |
| `(sqlite-version p)` | SQLite version compiled into libsqltree |
| `(close p)` | idempotent; parser also implements `java.io.Closeable` |

### parseq.jdbc

```clojure
(require '[parseq.jdbc :as pqj]
         '[parseq.core :as pq])

(with-open [parser (pqj/parser-from conn)]
  (pq/parse parser "SELECT ..."))
```

`parser-from` replays the connection's `sqlite_schema` DDL into a
scratch in-memory session, so parses resolve exactly what the live
database would (views, indexes, generated columns, triggers included).
`refresh!` re-replays after migrations; it is keyed on
`PRAGMA schema_version` and swaps in a fresh session, because a schema
can shrink and executed DDL cannot drop what an earlier replay created.
The connection can be closed as soon as the parser is created.

## Tree reference (selected nodes)

Statements are envelopes: `{:stmt <node> :sql "raw text" :explain 0|1|2}`
(or `{:node :error ...}` for the failing statement of a batch).

| node | keys |
|---|---|
| `:select` | `:distinct`, `:columns`, `:from`, `:where`, `:group-by`, `:having`, `:order-by`, `:limit`, `:windows`, `:ctes` |
| `:compound` | `:op :union/:except/:intersect/:union-all`, `:left`, `:right`; trailing `:order-by`/`:limit` (they bind to the compound) |
| `:values` | `:rows` — includes rows SQLite folds into its VALUES co-routine (captured before they are freed) |
| `:table` (FROM) | `:name`, `:schema`, `:alias`, `:cursor`, resolved `:columns`, `:join {:type [:left :outer] :on/:using}`, `:indexed-by`, `:not-indexed` |
| `:subquery` (FROM) | as above plus `:select`; `:view true` when it is an expanded view, `:cte true` for CTEs |
| `:table-function` | `:name`, `:args` |
| `:column` | `:table`, `:schema`, `:name`, `:idx` (`-1` + `:rowid true` for rowid/INTEGER PRIMARY KEY), `:cursor`, `:affinity`, `:on-join :outer/:inner` (provenance from a join's ON/USING) |
| `:literal` | `:kind :integer/:float/:string/:blob/:boolean/:null`, `:value` |
| `:parameter` | `:index` (1-based), `:name` for named |
| `:binop` / `:unop` | `:op` (`:+`, `:=`, `:and`, ...) |
| `:function` / `:agg-function` | `:name`, `:args`, `:distinct`, aggregate `:order-by`, `:window`, `:filter` |
| `:scalar-subquery` / `:exists` | `:select`, `:correlated` |
| `:in` | `:lhs`, `:rhs :select/:list`, `:values` |
| `:case`, `:cast`, `:collate`, `:between`, `:vector`, `:truth`, `:raise` | as expected |
| `:window` | `:name`, `:base`, `:partition-by`, `:order-by`, `:frame :rows/:range/:groups`, `:bounds`, `:exclude`, `:filter` |
| `:cte` | `:name`, `:columns`, `:materialized`, `:recursive`, `:query` |
| `:insert` | `:into`, `:columns` (with resolved `:idx`), `:source`, `:on-conflict [{:target :set :action}]`, `:or`, `:returning` |
| `:update` | `:target`, `:from`, `:set [{:column :idx :expr}]`, `:where`, `:returning` |
| `:delete` | `:from`, `:where`, `:returning` |
| `:create-table` | `:table` — columns with `:type/:affinity/:pk/:not-null/:collation/:default/:as-expr/:generated`, `:checks`, `:foreign-keys`, `:indexes`, `:without-rowid`, `:strict`, `:rowid-alias` |
| `:create-index` | `:table`, `:columns` (mapped to schema indexes), `:where` (partial), `:unique`, `:on-conflict` |
| `:create-view`, `:create-trigger` | trigger `:timing/:event/:update-of/:when/:steps` with NEW/OLD as `:trigger-column {:row :new/:old :name :idx}` |
| `:pragma`, `:begin`, `:commit`, `:rollback`, `:savepoint`, `:release`, `:rollback-to`, `:attach`, `:detach`, `:analyze`, `:vacuum`, `:reindex`, `:drop-*`, `:rename-*`, `:add-column`, `:alter-*` | leaf statements with names/args |
| `:other` | any verb without a deep dump; carries the raw `:sql` |
| `:error` | `:message`, `:offset` (byte offset into the input) |

Every expression also carries `:ofst` (byte offset of its first token)
where SQLite records one.

## Fidelity notes and limits

These are properties of dumping SQLite's own resolved structures —
documented so nothing surprises you:

- **ON clauses appear in `:where`.** SQLite's resolver moves ON/USING
  conditions into the WHERE tree; each column keeps `:on-join
  :outer/:inner` provenance.
- **Window frames on frame-ignoring functions are normalized.**
  `row_number() OVER (... 1 PRECEDING ...)` becomes the default frame
  because SQLite itself rewrites it; use `sum()`-style functions to see
  explicit frames. This is SQLite behavior, not lossy dumping.
- **UPDATE ... FROM** resolves SET expressions against the combined
  target+FROM `SrcList` directly, rather than through the synthesized
  SELECT SQLite uses internally; the resolution result is equivalent.
- **Trigger steps, column DEFAULT/AS and CHECK exprs** are stored by
  SQLite as reduced-size packed duplicates. parseq always resolves and
  dumps full-size copies; SQLite's own fire-time code does the same.
- **Multi-row constant VALUES** are compiled to a co-routine by the
  parser and rows 2+ are freed at parse time; parseq captures them
  before deletion so `:values` still lists every row.
- Deep trees cover the verbs listed above. Rarer verbs (ATTACH,
  ANALYZE, REINDEX, ...) are leaf nodes with their arguments; CREATE
  VIRTUAL TABLE currently dumps as `:other`.
- One statement batch per `parse` call runs under a single SQLite
  `Parse`; parsing is serialized per session (matching SQLite's own
  threading model — one connection, one parser).

## Repo layout

```
native/
  vendor/fetch.sh       pinned amalgamation download (SHA3-256)
  patch/parsecov.patch  the 29 hooks (apply with `patch -p0`)
  src/sqltree_hooks.h   hook contract
  src/sqltree.c         serializer + resolver driver + sqt_*/JNI
  src/sqltree_all.c     the single TU: amalgamation + serializer
  test/sqltree_smoke.c  C-level smoke suite (runs before any JVM)
src/parseq/
  native.clj            binding protocol + impl selection
  native/panama.clj     java.lang.foreign bindings
  native/jni.clj        JNI bindings via parseq.sqltree.Sqltree
  core.clj              Parser, load-schema, parse, close
  jdbc.clj              schema replay from a live connection
java/parseq/sqltree/    the JNI glue class
test/parseq/            resolution suite (runs under both bindings)
```

## License

The SQLite amalgamation is public domain (sqlite.org). parseq's own
code follows the repository license.
