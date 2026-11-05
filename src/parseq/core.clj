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
    (def p (pq/parser))
    (pq/load-schema p conn)   ; any java.sql Connection to a SQLite db
    (pq/parse p \"SELECT b FROM t WHERE a > ?\")

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
    :impl  :panama (default, FFM) or :jni
    :path  SQLite database path (default \":memory:\"). A file path
           resolves against that file's schema as opened; parsing
           never executes or mutates anything.

  Parsing always happens in capture mode; nothing you parse here is
  executed. Use load-schema to make names resolvable."
  (^Parser [] (parser nil))
  (^Parser [opts]
   (let [impl (or (:impl opts) :panama)
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

(defn parse
  "Parse `sql` (one or more statements) and return the resolved tree:

    {:sqlite \"3.54.1\"
     :statements [{:stmt {...} :sql \"SELECT ...\" :explain 0}]}

  A statement that fails to parse appears as
  {:node :error :message \"..\" :offset 42}. Never throws for bad SQL;
  throws only for infrastructure failures (missing library, OOM)."
  [^Parser p sql]
  (ensure-open p "parse")
  (edn/read-string (native/native-parse (:native p) (handle p) sql)))

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
