(ns parseq.core
  "Fully-resolved syntax trees for SQLite SQL.

  Parses SQL through SQLite's own parser and name resolver (a vendored
  amalgamation with parse-coverage hooks) and returns unambiguous,
  machine-readable trees as plain Clojure data. Every column reference
  is resolved to its table, schema, cursor and column index; views and
  '*' are expanded the way SQLite itself resolves them; parse errors
  come back as data with byte offsets.

  Quickstart:

    (require '[parseq.core :as pq])
    (pq/init! conn)          ; any java.sql Connection to a SQLite db
    (pq/parse \"SELECT b FROM t WHERE a > ?\")
    (pq/refresh-schema!)     ; after migrations on that connection

  The explicit surface — (parser opts), (load-schema p schema),
  (parse p sql) — remains for juggling several parsers at once;
  init! installs one globally.

  See README for the node reference and the exact resolution stage the
  dump reflects (post-resolution, pre-optimizer)."
  (:require [clojure.edn :as edn]
            [parseq.native :as native])
  (:import (java.sql Connection)))

;; The native handle lives in an atom so close is a one-shot: the swap
;; nils it and the winner performs the native close. Double close and
;; use-after-close both degrade safely.
(declare close)

(defrecord Parser [native impl ^clojure.lang.Atom session]
  java.io.Closeable
  (close [this] (close this)))

(defn- handle
  "The live native handle, or nil once closed."
  [^Parser p]
  (:handle @(:session p)))

(defn parser
  "Create a parser session.

  Options:
    :impl  :panama (FFM, needs a recent JVM) or :jni; the default is
           whichever binding loads on this JVM, FFM preferred
    :path  SQLite database path (default \":memory:\"). A file path
           resolves against that file's schema as opened; parsing
           never executes or mutates anything.

  Parsing always happens in capture mode; nothing you parse here is
  executed. Use load-schema to make names resolvable."
  (^Parser [] (parser nil))
  (^Parser [opts]
   (let [impl (or (:impl opts) (native/default-impl))
         n (native/make impl)
         h (native/native-open n (or (:path opts) ":memory:"))]
     (->Parser n impl (atom {:handle h})))))

(defn- ensure-open
  [^Parser p what]
  (when (nil? (handle p))
    (throw (ex-info (str "parser is closed: " what) {}))))

(defn schema-ddl
  "The CREATE statements of every object in `conn`'s schema,
  concatenated in sqlite_schema order (tables and indexes interleaved
  the way SQLite stores them). This is the DDL load-schema replays
  for a connection."
  ^String [^Connection conn]
  (with-open [stmt (.createStatement conn)
              rs (.executeQuery stmt
                    (str "SELECT sql FROM sqlite_schema"
                         " WHERE sql IS NOT NULL AND name NOT LIKE 'sqlite_autoindex_%'"
                         " ORDER BY rowid"))]
    (let [sb (StringBuilder.)]
      (loop []
        (when (.next rs)
          (doto sb
            (.append (.getString rs 1))
            (.append ";\n"))
          (recur)))
      (.toString sb))))

(defn load-schema
  "Load a schema into the session so subsequent parses resolve against
  it. `schema` is either a java.sql Connection to a SQLite database —
  its sqlite_schema DDL is read and replayed, and the connection is
  never written — or a string of DDL executed as-is. Returns nil on
  success or {:error \"...\"} when sqlite rejects the DDL. This is the
  only operation that touches database state (the session's own
  scratch database)."
  [^Parser p schema]
  (ensure-open p "load-schema")
  (let [ddl (if (instance? Connection schema) (schema-ddl schema) schema)]
    (when-let [err (native/native-load-schema (:native p) (handle p) ddl)]
      (edn/read-string err))))

;; State for the init!/parse/refresh-schema! surface: the installed
;; parser and the connection its schema was replayed from. parse needs
;; only the parser; refresh-schema! re-reads the connection, so it
;; must stay open until close!.
(def ^:private session (atom nil))

(defn- require-session
  "The installed parser, or a throw pointing at init!."
  []
  (or (:parser @session)
      (throw (ex-info "parseq is not initialized; call (init! conn)" {}))))

(defn parse
  "Parse `sql` (one or more statements) and return the resolved tree:

    {:sqlite \"3.54.1\"
     :statements [{:stmt {...} :sql \"SELECT ...\" :explain 0}]}

  A statement that fails to parse appears as
  {:node :error :message \"..\" :offset 42}. Never throws for bad SQL;
  throws only for infrastructure failures (missing library, OOM). The
  one-argument form parses against the parser installed by init!."
  ([sql] (parse (require-session) sql))
  ([^Parser p sql]
   (ensure-open p "parse")
   (edn/read-string (native/native-parse (:native p) (handle p) sql))))

(defn sqlite-version
  "SQLite version compiled into the native library."
  [^Parser p]
  (native/native-version (:native p)))

(defn close
  "Release the session. The parser is unusable afterwards. Idempotent."
  [^Parser p]
  (let [[old _] (swap-vals! (:session p) #(assoc % :handle nil))]
    (when-some [h (:handle old)]
      (native/native-close (:native p) h)))
  nil)

(defn reset-handle!
  "Swap in a fresh native handle (used by parseq.jdbc/refresh! to
  replay a schema from scratch). Closes the previous one. Internal."
  [^Parser p new-h]
  (let [[old _] (swap-vals! (:session p) #(assoc % :handle new-h))]
    (when-some [h (:handle old)]
      (native/native-close (:native p) h))))

(defn replay-schema!
  "Replay `conn`'s schema into `parser` on a fresh native session,
  replacing whatever it held — schemas can shrink, and exec'd DDL
  cannot drop what an earlier replay created. Throws on a rejected
  replay. Workhorse of refresh-schema! and parseq.jdbc/refresh!."
  [^Parser p ^Connection conn]
  (let [n (:native p)]
    (let [h (native/native-open n ":memory:")]
      (when-let [err (native/native-load-schema n h (schema-ddl conn))]
        (native/native-close n h)
        (throw (ex-info "schema replay failed" {:error (edn/read-string err)})))
      (reset-handle! p h))
    p))

(defn init!
  "Install `conn` as the schema source for the one-argument parse and
  return the parser. The connection's sqlite_schema is replayed into a
  fresh session; the connection is only ever read (here and on
  refresh-schema!) but must stay open until close!. Calling init!
  again closes the parser it replaces. `opts` are as for parser."
  ([conn] (init! conn nil))
  ([conn opts]
   (let [p (parser opts)]
     (when-let [err (load-schema p conn)]
       (close p)
       (throw (ex-info "schema replay failed" err)))
     (let [[old _] (swap-vals! session assoc :parser p :conn conn)]
       (when-some [old-p (:parser old)]
         (close old-p)))
     p)))

(defn refresh-schema!
  "Rebuild the installed parser's schema from the connection init!
  received, on a fresh native session. Unconditional: calling it is
  the explicit signal that the schema changed. (parseq.jdbc/refresh!
  is the schema_version-gated variant over an explicit parser.)"
  []
  (require-session)
  (let [{:keys [parser conn]} @session]
    (replay-schema! parser conn)))

(defn close!
  "Close the parser installed by init! and forget its connection.
  Idempotent; explicit-handle parsers are unaffected."
  []
  (let [[old _] (swap-vals! session (constantly nil))]
    (when-some [p (:parser old)]
      (close p))
    nil))
