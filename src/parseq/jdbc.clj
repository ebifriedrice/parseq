(ns parseq.jdbc
  "Resolve SQL against the schema of an existing SQLite database.

  parseq's native parser resolves names against its own session
  database; this namespace bridges a JDBC connection (org.xerial
  sqlite-jdbc or anything else speaking SQLite) into that session by
  replaying the connection's sqlite_schema DDL. The parser then
  resolves exactly what the live database would.

    (require '[parseq.core :as pq]
              '[parseq.jdbc :as pqj])
    (with-open [parser (pqj/parser-from conn)]
      (pq/parse parser \"SELECT ...\"))

  The parser never connects to the database itself, so the connection
  can be closed as soon as the schema is loaded."
  (:require [parseq.core :as pq])
  (:import (java.sql Connection)))

(defn schema-version
  "PRAGMA schema_version of the connection; bump this to detect when a
  parser's replayed schema has gone stale."
  [^Connection conn]
  (with-open [stmt (.createStatement conn)
              rs (.executeQuery stmt "PRAGMA schema_version")]
    (when (.next rs)
      (.getInt rs 1))))

(defn refresh!
  "Replay the connection's schema into `parser`, replacing whatever it
  held. Safe to call repeatedly; a no-op when PRAGMA schema_version is
  unchanged since the last load for this connection. Returns the
  parser (with the schema version in metadata)."
  [parser ^Connection conn]
  (let [v (schema-version conn)]
    (if (= v (::schema-version (meta parser)))
      parser
      (vary-meta (pq/replay-schema! parser conn)
                 assoc ::schema-version v))))

(defn parser-from
  "A parser whose schema mirrors `conn` at creation time. Use refresh!
  after migrations or DDL on the connection."
  ([^Connection conn]
   (parser-from conn nil))
  ([^Connection conn opts]
   (refresh! (pq/parser (assoc (or opts {}) :path ":memory:")) conn)))
