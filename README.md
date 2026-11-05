# parseq

Unambiguous, fully-resolved syntax trees for SQLite SQL — built on
SQLite's own parser and name resolver, exposed to Clojure through
both JNI and Panama (FFM) bindings, and resolved against a live
database's schema through any JDBC connection.

```clojure
(require '[parseq.core :as pq])

(pq/init! conn)  ; any java.sql Connection to a SQLite database

(pq/parse "SELECT u.name FROM users u WHERE u.id > ?1")

(pq/refresh-schema!)  ; after migrations on that connection
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
            │   + native/patch/parsecov.patch (28 hunks)     │
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
```

- **The parser and resolver are SQLite's own.** `native/vendor/`
  carries SQLite 3.54.1 (`sqlite3.h` byte-for-byte upstream,
  `sqlite3.c` upstream plus `parsecov.patch` — a reviewable 28-hunk
  diff that inserts guarded hook calls at the boundary where a
  statement is fully name-resolved but code generation has not
  started; see `native/vendor/README.md` for provenance and how to
  re-derive or bump the vendored copy). Outside `parse` the library
  behaves exactly like stock SQLite.
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

## Installing

```clojure
net.clojars.ebifriedrice/parseq {:mvn/version "0.1.0"}
```

Released jars carry precompiled `libsqltree` for linux/amd64,
linux/arm64, macos/arm64 (Apple Silicon) and windows/amd64, so the
usual dependency line is all you need on any of those. Other
platforms (Intel Macs included) build the library locally instead —
see the next section; `-Dparseq.lib` or `java.library.path` will
find it.

## Building and running

Requirements: CMake ≥ 3.20, Ninja, a C compiler, Clojure CLI, and a
JVM — the JNI binding loads anywhere (its glue class is compiled at
release 8), the FFM binding needs JDK 22+ (`--enable-native-access`;
21 with preview flags). With no `:impl` given, parseq picks whichever
binding the JVM can load, FFM preferred. The SQLite sources are
vendored in the tree, so builds are fully offline.

```sh
clojure -T:build native   # build libsqltree from the vendored sources
clojure -T:build javac    # compile the JNI glue class
clojure -T:build test     # both of the above, then the full suite
```

`clojure -M:dev` gives you a REPL with `java.library.path` and
`--enable-native-access` set. The native library is found via
`-Dparseq.lib=/path/to/libsqltree.so`, then `java.library.path`,
then the copy embedded in the parseq jar for the running platform
(extracted to a temp file at first use).

## Releasing

Deploys happen only on version bumps: push a tag and CI does the
rest.

```sh
git tag v0.1.0 && git push origin v0.1.0
```

The release workflow builds the native library on every shipped
arch (linux/amd64, linux/arm64, macos/arm64, windows/amd64), runs
the C smoke test per arch, assembles one jar carrying all four,
proves the jar loads its natives on a bare JVM, then publishes to
Clojars under the tag's version (`v0.1.0` → `0.1.0`). Dispatching
the workflow by hand runs everything except the deploy — a dry run.

## The API

### parseq.core

| form | meaning |
|---|---|
| `(init! conn opts?)` | install the connection for one-arg `parse`; its schema is replayed, and the conn is only ever read but held until `close!` |
| `(parse sql)` | parse against the installed parser |
| `(refresh-schema!)` | rebuild the installed schema cache from the conn — unconditional, the explicit signal it changed |
| `(close!)` | close the installed parser and forget the conn |
| `(parser {:impl :panama \| :jni :path "..."})` | new explicit session; default binding is whichever loads on this JVM (FFM preferred), default path `:memory:` |
| `(load-schema p schema)` | a `java.sql.Connection` — its `sqlite_schema` DDL is read (never written) and replayed — or a DDL string for hand-built schemas; returns `{:error "..."}` on rejection |
| `(schema-ddl conn)` | the connection's CREATE statements, in `sqlite_schema` order |
| `(replay-schema! p conn)` | rebuild an explicit parser's schema from a connection on a fresh session; throws on rejection |
| `(parse p sql)` | parse one or many statements; returns the tree; never throws for bad SQL |
| `(sqlite-version p)` | SQLite version compiled into libsqltree |
| `(close p)` | idempotent; parser also implements `java.io.Closeable` |

## Tree reference (selected nodes)

Statements are envelopes: `{:stmt <node> :sql "raw text" :explain 0|1|2}`
(or `{:node :error ...}` for the failing statement of a batch).

| node | keys |
|---|---|
| `:select` | `:distinct`, `:columns`, `:from`, `:where`, `:group-by`, `:having`, `:order-by`, `:limit`, `:windows`, `:ctes` |
| `:compound` | `:op :union/:except/:intersect/:union-all`, `:left`, `:right`; trailing `:order-by`/`:limit` (they bind to the compound) |
| `:values` | `:rows` — the rows SQLite's own tree still holds (constant rows 2+ are freed at parse time) |
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
  parser and rows 2+ are freed at parse time, so `:values` lists only
  the first row. This is SQLite's own tree, unembellished.
- Deep trees cover the verbs listed above. Rarer verbs (ATTACH,
  ANALYZE, REINDEX, ...) are leaf nodes with their arguments; CREATE
  VIRTUAL TABLE currently dumps as `:other`.
- One statement batch per `parse` call runs under a single SQLite
  `Parse`; parsing is serialized per session (matching SQLite's own
  threading model — one connection, one parser).

## Repo layout

```
native/
  vendor/sqlite3.c,.h   vendored 3.54.1; .c carries the patch
  vendor/README.md      provenance: version, zip hash, re-derivation
  patch/parsecov.patch  the 28 hooks = the delta vs upstream
  src/sqltree_hooks.h   hook contract
  src/sqltree.c         serializer + resolver driver + sqt_*/JNI
  src/sqltree_all.c     the single TU: amalgamation + serializer
  test/sqltree_smoke.c  C-level smoke suite (runs before any JVM)
src/parseq/
  native.clj            binding protocol + impl selection
  native/panama.clj     java.lang.foreign bindings
  native/jni.clj        JNI bindings via parseq.sqltree.Sqltree
  core.clj              init!/parse/refresh-schema! dynamic surface,
                        Parser, load-schema, replay-schema!, close
java/parseq/sqltree/    the JNI glue class + its native loader
test/parseq/            resolution suite (runs under both bindings)
```

## License

The SQLite amalgamation is public domain (sqlite.org). parseq's own
code follows the repository license.
