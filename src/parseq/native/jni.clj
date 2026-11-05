(ns parseq.native.jni
  "Classic-JNI binding through the parseq.sqltree.Sqltree glue class.

  Semantically identical to parseq.native.panama; useful where FFM is
  unavailable or undesired. Requires the Java class compiled by the
  build task ('clojure -T:build javac'); the library is located by
  parseq.sqltree.NativeLoader: -Dparseq.lib, then java.library.path,
  then the copy embedded in the jar for this platform."
  (:require [parseq.native :as native])
  (:import (parseq.sqltree Sqltree)))

(deftype JniNative []
  native/Sqltree
  (native-open [_ path]
    (let [h (Sqltree/open (str (or path ":memory:")))]
      (when (zero? h)
        (throw (ex-info "sqt_open failed" {:path path})))
      h))
  (native-close [_ handle]
    (Sqltree/close handle)
    nil)
  (native-parse [_ handle sql]
    (or (Sqltree/parse handle (str sql))
        (throw (ex-info "sqt_parse returned NULL (out of memory?)" {:sql sql}))))
  (native-load-schema [_ handle ddl]
    (Sqltree/loadSchema handle (str ddl)))
  (native-version [_]
    (Sqltree/version)))
