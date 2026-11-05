(ns parseq.jdbc-test
  (:require [clojure.test :refer [deftest is testing use-fixtures]]
            [parseq.core :as pq]
            [parseq.jdbc :as pqj])
  (:import (org.sqlite SQLiteDataSource)))

(defn- mem-conn ^java.sql.Connection []
  (-> (doto (SQLiteDataSource.) (.setUrl "jdbc:sqlite::memory:"))
      (.getConnection)))

(def ^:private ddl
  (str "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL);"
       "CREATE TABLE orders (oid INTEGER PRIMARY KEY,"
       " uid INT REFERENCES users(id) ON DELETE CASCADE, amt REAL);"
       "CREATE INDEX idx_orders_uid ON orders(uid);"
       "CREATE VIEW big AS SELECT * FROM orders WHERE amt > 100;"))

(deftest parser-from-resolves-like-the-database
  (with-open [conn (mem-conn)
              st (.createStatement conn)]
    (.executeUpdate st ddl)
    (with-open [parser (pqj/parser-from conn)]
      (let [tree (-> (pq/parse parser
                               "SELECT u.name FROM users u JOIN orders o ON o.uid = u.id")
                     :statements first :stmt)]
        (is (= "users" (-> tree :columns first :expr :table)))
        (is (= [{:name "users" :alias "u"} {:name "orders" :alias "o"}]
               (mapv #(select-keys % [:name :alias]) (:from tree))))
        (testing "view from the live schema expands"
          (is (= ["oid" "uid" "amt"]
                 (->> (pq/parse parser "SELECT * FROM big")
                      :statements first :stmt :columns
                      (mapv (comp :name :expr))))))))))

(deftest refresh-picks-up-migrations
  (with-open [conn (mem-conn)
              st (.createStatement conn)]
    (.executeUpdate st "CREATE TABLE t (a INT);")
    (with-open [parser (pqj/parser-from conn)]
      (is (= :error (-> (pq/parse parser "SELECT b FROM t")
                        :statements first :node)))
      (.executeUpdate st "ALTER TABLE t ADD COLUMN b TEXT;")
      (pqj/refresh! parser conn)
      (is (= "b"
             (-> (pq/parse parser "SELECT b FROM t")
                 :statements first :stmt :columns first :expr :name))))))

(deftest schema-ddl-filters-implicit-objects
  (with-open [conn (mem-conn)
              st (.createStatement conn)]
    (.executeUpdate st "CREATE TABLE t (a TEXT UNIQUE);")
    (let [ddl (pq/schema-ddl conn)]
      (is (re-find #"CREATE TABLE t" ddl))
      (is (not (re-find #"sqlite_autoindex" ddl))
          "autoindexes are implicit in their table and must not be replayed"))))
