(ns parseq.core
  "Fully-resolved syntax trees for SQLite SQL.

  Parses SQL through SQLite's own parser and name resolver (vendored
  amalgamation with parse-coverage hooks) and returns unambiguous,
  machine-readable trees as plain Clojure data.")
