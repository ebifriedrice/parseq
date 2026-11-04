(ns parseq.native
  "The contract every parseq native binding implements.

  parseq ships two interchangeable bindings to the same libsqltree
  shared library: `parseq.native.panama` (java.lang.foreign, the
  modern FFM API) and `parseq.native.jni` (classic JNI through a small
  generated Java class). Both talk to the identical C ABI, so the
  choice is a deployment detail, not a semantic one."
  (:refer-clojure :exclude [parse]))

(defprotocol Sqltree
  "Lifecycle and calls onto libsqltree."

  (native-open [this path]
    "Open a session over the SQLite database at `path` (\":memory:\"
    for scratch). Returns an opaque handle.")

  (native-close [this handle]
    "Release a session and its database connection.")

  (native-parse [this handle sql]
    "Parse `sql` in capture mode. Returns the EDN text produced by the
    serializer: {:sqlite \"..\" :statements [..]}. Parse never executes
    or mutates anything.")

  (native-load-schema [this handle ddl]
    "Execute `ddl` against the session's database so later parses
    resolve names against it. Returns nil, or an EDN error map when
    the DDL is rejected.")

  (native-version [this]
    "The SQLite version string compiled into libsqltree."))

(defn available
  "Impls that can load in the current JVM, in preference order."
  []
  (keep (fn [[k ctor-var]]
          (when-let [impl (try (when ctor-var (ctor-var))
                               (catch Throwable _ nil))]
            (when (try (native-version impl) true
                       (catch Throwable _ false))
              k)))
        {:panama (try (require 'parseq.native.panama)
                      (resolve 'parseq.native.panama/->PanamaNative)
                      (catch Throwable _ nil))
         :jni    (try (require 'parseq.native.jni)
                      (resolve 'parseq.native.jni/->JniNative)
                      (catch Throwable _ nil))}))

(defn make
  "Instantiate a binding. `impl` is :panama or :jni; defaults to the
  first available, Panama preferred."
  ([]
   (let [[k & _] (available)]
     (when-not k
       (throw (ex-info "no parseq native implementation could load; is libsqltree on java.library.path?"
                       {:available []})))
     (make k)))
  ([impl]
   (case impl
     :panama (do (require 'parseq.native.panama)
                 ((resolve 'parseq.native.panama/->PanamaNative)))
     :jni    (do (require 'parseq.native.jni)
                 ((resolve 'parseq.native.jni/->JniNative))))))
