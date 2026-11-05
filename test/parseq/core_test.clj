(ns parseq.core-test
  "Resolution suite, run against every binding implementation.

  Each test parses SQL against a fixed schema and asserts on the
  resolved tree: the point is not that these shapes are pretty, but
  that resolution is SQLite's own - columns pinned to table/schema/
  cursor/index, views and '*' expanded, NEW/OLD named inside
  triggers - and that both impls agree on every tree."
  (:require [clojure.test :refer [deftest is testing]]
            [parseq.core :as pq]
            [parseq.native :as native])
  (:import (org.sqlite SQLiteDataSource)))

(def ^:private schema
  (str "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL,"
       " email TEXT UNIQUE, score REAL DEFAULT 0.0);"
       "CREATE TABLE orders (oid INTEGER PRIMARY KEY REFERENCES users(id),"
       " uid INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
       " total NUMERIC CHECK (total >= 0),"
       " created_at TEXT DEFAULT (datetime('now')),"
       " double_total REAL GENERATED ALWAYS AS (total*2) VIRTUAL);"
       "CREATE TABLE wr (k TEXT PRIMARY KEY, v INT) WITHOUT ROWID;"
       "CREATE INDEX idx_orders_uid ON orders(uid DESC) WHERE uid > 10;"
       "CREATE VIEW v_tot AS SELECT uid, sum(total) AS s FROM orders GROUP BY uid;"))

(defn- each-impl [f]
  (doseq [impl (native/available)]
    (testing (str "impl " impl)
      (with-open [p (pq/parser {:impl impl})]
        (pq/load-schema p schema)
        (f p)))))

(defmacro deftest-both [name [p] & body]
  `(deftest ~name (each-impl (fn [~p] ~@body))))

(defn- stmt
  "The n-th parsed statement (default first)."
  [p sql & [n]]
  (nth (:statements (pq/parse p sql)) (or n 0)))

(defn- first-column-expr [p sql]
  (-> (stmt p sql) :stmt :columns first :expr))

(deftest load-schema-from-connection
  (with-open [conn (-> (doto (SQLiteDataSource.) (.setUrl "jdbc:sqlite::memory:"))
                       (.getConnection))
              st (.createStatement conn)]
    (.executeUpdate st schema)
    (each-impl
      (fn [p]
        (pq/load-schema p conn)
        (is (= {:table "users" :schema "main" :name "name" :idx 1 :affinity :text}
               (select-keys (first-column-expr p "SELECT u.name FROM users u")
                            [:table :schema :name :idx :affinity])))
        (is (= :error (-> (stmt p "SELECT nope FROM users") :node))
            "names resolve against the replayed schema, unknown ones error")))))

(deftest dynamic-surface
  (with-open [conn (-> (doto (SQLiteDataSource.) (.setUrl "jdbc:sqlite::memory:"))
                       (.getConnection))
              st (.createStatement conn)]
    (.executeUpdate st schema)
    (try
      (pq/init! conn)
      (is (= {:table "users" :schema "main" :name "name" :idx 1 :affinity :text}
             (-> (pq/parse "SELECT u.name FROM users u")
                 :statements first :stmt :columns first :expr
                 (select-keys [:table :schema :name :idx :affinity]))))
      (.executeUpdate st "ALTER TABLE users ADD COLUMN extra TEXT;")
      (is (= :error (-> (pq/parse "SELECT extra FROM users")
                        :statements first :node))
          "the replayed schema is a cache; it does not track the conn")
      (pq/refresh-schema!)
      (is (= "extra" (-> (pq/parse "SELECT extra FROM users")
                         :statements first :stmt :columns first :expr :name)))
      (pq/close!)
      (is (thrown? Exception (pq/parse "SELECT 1"))
          "parse without an installed parser fails fast")
      (finally
        (pq/close!)))))

(deftest-both column-resolution [p]
  (testing "qualified column resolves to table, schema, index, affinity"
    (is (= {:table "users" :schema "main" :name "name" :idx 1 :affinity :text}
           (select-keys (first-column-expr p "SELECT u.name FROM users u")
                        [:table :schema :name :idx :affinity]))))
  (testing "integer primary key resolves to the rowid, keeping its name"
    (is (= {:name "id" :idx -1 :table "users" :rowid true}
           (select-keys (first-column-expr p "SELECT u.id FROM users u")
                        [:name :idx :table :rowid]))))
  (testing "bare column resolves through an alias"
    (is (= "users"
           (:table (first-column-expr p "SELECT name FROM users AS x"))))))

(deftest-both star-expansion [p]
  (is (= ["oid" "uid" "total" "created_at" "double_total"]
         (->> (stmt p "SELECT * FROM orders")
              :stmt :columns (mapv :name)))
      "* expands to every column in declared order")
  (is (= {:table "users" :name "id" :rowid true}
         (-> (first-column-expr p "SELECT u.* FROM users u")
             (select-keys [:table :name :rowid])))
      "qualified * expands against that table; the rowid alias keeps
       its declared name"))

(deftest-both join-resolution [p]
  (let [from (-> (stmt p "SELECT 1 FROM users u LEFT JOIN orders o ON o.uid = u.id")
                 :stmt :from)]
    (is (= 2 (count from)))
    (is (= "orders" (-> from second :name)))
    (is (= [:left :outer] (-> from second :join :type)))
    (testing "the resolver folds ON into WHERE, keeping provenance"
      (let [where (-> (stmt p "SELECT 1 FROM users u LEFT JOIN orders o ON o.uid = u.id")
                      :stmt :where)]
        (is (= := (:op where)))
        (is (= {:node "column" :table "orders" :name "uid" :idx 1}
               (-> where :lhs
                   (select-keys [:node :table :name :idx])
                   (update :node name))))
        (is (= {:name "id" :table "users" :rowid true}
               (-> where :rhs (select-keys [:name :table :rowid]))))
        (is (= :outer (:on-join (:lhs where)))
            "ON-clause columns carry their join provenance")))))

(deftest-both using-join [p]
  (is (= ["id"]
         (-> (stmt p "SELECT 1 FROM users JOIN users u2 USING (id)")
             :stmt :from second :join :using))))

(deftest-both subquery-and-view [p]
  (testing "scalar subquery keeps its own resolved cursors"
    (is (= {:node :scalar-subquery}
           (-> (stmt p "SELECT (SELECT max(id) FROM users) x")
               :stmt :columns first :expr
               (select-keys [:node])))))
  (testing "a view in FROM is the expanded subquery sqlite resolves against"
    (let [from (-> (stmt p "SELECT s FROM v_tot WHERE uid = 1") :stmt :from)]
      (is (:view (first from)))
      (is (= :subquery (:node (first from))))
      (is (= ["uid" "s"] (->> from first :columns (mapv :name)))))))

(deftest-both cte-and-compound [p]
  (let [tree (-> (stmt p "WITH c(x) AS (SELECT 1 UNION SELECT 2) SELECT x FROM c")
                 :stmt)]
    (is (= ["c"] (mapv :name (:ctes tree))))
    (is (= :union (-> tree :ctes first :query :op)))
    (is (= {:node :subquery :name "c" :cte true}
           (-> tree :from first (select-keys [:node :name :cte]))))))

(deftest-both window-function [p]
  (let [w (-> (stmt p "SELECT sum(total) OVER (PARTITION BY uid ORDER BY total ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) FROM orders")
              :stmt :columns first :expr :window)]
    (is (= {:node :window :frame :rows} (select-keys w [:node :frame])))
    (is (= {:start :expr :start-dir :preceding :end :current}
           (select-keys (:bounds w) [:start :start-dir :end])))))

(deftest-both values-form [p]
  (let [v (-> (stmt p "VALUES (3),(4),(5)") :stmt :from first :select)]
    (is (= :values (:node v)))
    (is (= [[3]]
           (mapv (fn [row] (mapv (comp :int-value :expr) row)) (:rows v)))
        "sqlite compiles constant rows 2+ into its co-routine and
         frees them at parse time; the dump shows its own tree")))

(deftest-both insert-upsert-returning [p]
  (let [tree (-> (stmt p "INSERT INTO orders (uid, total) VALUES (1, 9.5) ON CONFLICT (uid) DO UPDATE SET total = total + 1 RETURNING oid, total")
                 :stmt)]
    (is (= :insert (:node tree)))
    (is (= "orders" (:name (:into tree))))
    (is (= [{:name "uid" :idx 1} {:name "total" :idx 2}]
           (mapv #(select-keys % [:name :idx]) (:columns tree))))
    (is (= :values (-> tree :source :node)))
    (let [upsert (-> tree :on-conflict first)]
      (is (= {:node :upsert :action :do-update}
             (select-keys upsert [:node :action])))
      (is (= "uid" (-> upsert :target first :expr :name)))
      (is (= [{:column "total" :idx 2}]
             (mapv #(select-keys % [:column :idx]) (:set upsert)))))
    (is (= ["oid" "total"]
           (mapv (comp :name :expr) (:returning tree))))))

(deftest-both update-set-resolution [p]
  (let [tree (-> (stmt p "UPDATE users SET score = score + ? WHERE name LIKE 'a%'")
                 :stmt)]
    (is (= :update (:node tree)))
    (is (= [{:column "score" :idx 3}]
           (mapv #(select-keys % [:column :idx]) (:set tree))))
    (is (= {:node :column :table "users" :name "score"}
           (-> tree :set first :expr :lhs
               (select-keys [:node :table :name]))))
    (is (= {:node "parameter" :index 1}
           (-> tree :set first :expr :rhs
               (select-keys [:node :index])
               (update :node name))))))

(deftest-both delete-with-in [p]
  (let [tree (-> (stmt p "DELETE FROM orders WHERE uid IN (SELECT id FROM users)")
                 :stmt)]
    (is (= :delete (:node tree)))
    (is (= "select" (name (-> tree :where :rhs))))
    (is (= {:node :table :name "users"}
           (-> tree :where :select :from first
               (select-keys [:node :name]))))
    (is (= {:name "id" :table "users" :rowid true}
           (-> tree :where :select :columns first :expr
               (select-keys [:name :table :rowid]))))))

(deftest-both ddl-dump [p]
  (testing "create table resolves defaults, checks, generated columns, fks"
    (let [tree (-> (stmt p "CREATE TABLE q (a INT, b TEXT COLLATE NOCASE, c INT GENERATED ALWAYS AS (a+1) STORED, CHECK (a > 0), FOREIGN KEY (c) REFERENCES users(id) ON DELETE CASCADE)")
                   :stmt :table)]
      (is (= "q" (:name tree)))
      (is (= {:name "c" :generated :stored}
             (select-keys (->> tree :columns (filter #(= "c" (:name %))) first)
                          [:name :generated])))
      (is (= :+
             (:op (:as-expr (->> tree :columns (filter #(= "c" (:name %))) first)))))
      (is (= :binop (-> tree :checks first :expr :node))
          "check exprs resolve against the table")
      (is (= {:parent-table "users" :on-delete :cascade}
             (-> tree :foreign-keys first
                 (select-keys [:parent-table :on-delete]))))))
  (testing "without rowid and strict tables are flagged"
    (is (true? (-> (stmt p "CREATE TABLE t (a TEXT PRIMARY KEY) WITHOUT ROWID")
                   :stmt :table :without-rowid)))
    (is (true? (-> (stmt p "CREATE TABLE t (a INT) STRICT")
                   :stmt :table :strict))))
  (testing "create index maps indexed columns to schema indexes"
    (let [tree (-> (stmt p "CREATE INDEX i2 ON orders(total COLLATE NOCASE DESC)")
                   :stmt)]
      (is (= :create-index (:node tree)))
      (is (= [{:column "total" :idx 2 :dir :desc}]
             (mapv #(select-keys % [:column :idx :dir]) (:columns tree)))))))

(deftest-both trigger-new-old [p]
  (let [tree (-> (stmt p "CREATE TRIGGER g AFTER UPDATE OF name ON users BEGIN UPDATE users SET score = score + old.score WHERE id = new.id; END")
                 :stmt)]
    (is (= {:node :create-trigger :timing "after" :event "update"}
           (-> tree
               (select-keys [:node])
               (assoc :timing (name (:timing tree)) :event (name (:event tree))))))
    (is (= ["name"] (:update-of tree)))
    (is (= {:row :old :name "score" :idx 3}
           (-> tree :steps first :set first :expr :rhs
               (select-keys [:row :name :idx])))
        "old.score resolves to the trigger table column")))

(deftest-both error-reporting [p]
  (let [res (pq/parse p "SELECT 1; SELECT nope FROM users;")]
    (is (= :select (-> res :statements first :stmt :node)))
    (is (= {:node :error :message "no such column: nope"}
           (select-keys (second (:statements res)) [:node :message])))
    (is (pos-int? (:offset (second (:statements res)))))))

(deftest-both syntax-error-has-offset [p]
  (let [err (stmt p "SELECT FROM WHERE")]
    (is (= :error (:node err)))
    (is (string? (:message err)))
    (is (pos-int? (:offset err)))))

(deftest-both multi-statement-spans [p]
  (let [res (pq/parse p "-- c\n SELECT 1;/* m */ SELECT 2")]
    (is (= ["-- c\n SELECT 1" "SELECT 2"]
           (mapv :sql (:statements res))))))

(deftest-both explain-envelope [p]
  (let [res (pq/parse p "EXPLAIN SELECT 1; SELECT 2")]
    (is (= [1 nil] (mapv :explain (:statements res)))
        "EXPLAIN applies to exactly the following statement")))

(deftest-both misc-verbs [p]
  (is (= :pragma (-> (stmt p "PRAGMA main.foreign_keys = ON") :stmt :node)))
  (is (= :vacuum (-> (stmt p "VACUUM INTO 'x.db'") :stmt :node)))
  (is (= :begin (-> (stmt p "BEGIN IMMEDIATE") :stmt :node)))
  (is (= :commit (-> (stmt p "COMMIT") :stmt :node)))
  (is (= :savepoint (-> (stmt p "SAVEPOINT sp1") :stmt :node)))
  (is (= :drop-table (-> (stmt p "DROP TABLE IF EXISTS nosuch") :stmt :node))))

(deftest impls-agree
  (let [sqls ["SELECT * FROM users JOIN orders ON uid = id WHERE total > 1"
              "WITH c AS (SELECT 1 AS x) SELECT x FROM c UNION ALL SELECT 2"
              "INSERT INTO users (name) VALUES ('a') RETURNING id"]]
    (doseq [sql sqls]
      (let [results (for [impl (native/available)]
                      (with-open [p (pq/parser {:impl impl})]
                        (pq/load-schema p schema)
                        (pq/parse p sql)))]
        (when (> (count results) 1)
          (is (apply = results) "impls produce identical trees"))))))

(deftest default-binding-by-jvm
  (with-open [p (pq/parser)]
    (is (contains? (set (native/available)) (:impl p))
        "the zero-opts parser runs on whichever binding this JVM loads")))

(deftest close-is-one-shot
  (with-open [p (pq/parser)]
    (pq/close p)
    (pq/close p)                                    ; must not double-free
    (is (thrown? Exception (pq/parse p "SELECT 1")))))
