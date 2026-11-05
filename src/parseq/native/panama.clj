(ns parseq.native.panama
  "Panama FFM bindings to libsqltree's sqt_* C ABI.

  No Java glue for linking: handles are linked straight out of
  libsqltree with SymbolLookup/libraryLookup, so the only artifact
  this namespace needs is the shared library itself, found via the
  parseq.lib system property, java.library.path, or the copy embedded
  in the parseq jar for this platform (extracted to a temp file).

  Memory discipline: a session owns a shared arena that keeps its
  handle segment alive; each call uses a confined arena for argument
  strings. Result buffers are malloc'd by SQLite rather than by Java,
  so sqt_free is invoked on them explicitly after copying the text."
  (:require [clojure.java.io :as io]
            [parseq.native :as native])
  (:import (java.io File)
           (java.lang.foreign Arena FunctionDescriptor Linker Linker$Option
                              MemorySegment SymbolLookup ValueLayout)
           (java.lang.invoke MethodHandle)))

(defn- os-name
  "The <os> component of the embedded-natives resource layout; the
  same mapping parseq.sqltree.NativeLoader uses for JNI."
  []
  (condp re-find (.toLowerCase (System/getProperty "os.name" ""))
    #"win" "windows"
    #"(mac|darwin)" "macos"
    "linux"))

(defn- arch-name
  "The <arch> component of the embedded-natives resource layout."
  []
  (let [a (.toLowerCase (System/getProperty "os.arch" ""))]
    (cond
      (contains? #{"amd64" "x86_64"} a) "amd64"
      (contains? #{"aarch64" "arm64"} a) "arm64"
      :else a)))

(defn- embedded-path
  "Extract the jar's embedded libsqltree for this os/arch (resource
  parseq/native/<os>/<arch>/<lib>) to a temp file and return its
  path; nil when the classpath carries none. No load side effect:
  libraryLookup opens the extracted file directly."
  []
  (let [name (System/mapLibraryName "sqltree")
        url  (io/resource (format "parseq/native/%s/%s/%s"
                                  (os-name) (arch-name) name))]
    (when url
      (let [path (java.nio.file.Files/createTempFile
                  "libsqltree-" (subs name (.lastIndexOf name "."))
                  (make-array java.nio.file.attribute.FileAttribute 0))]
        (with-open [in (.openStream ^java.net.URL url)]
          (java.nio.file.Files/copy
            in path
            (into-array java.nio.file.StandardCopyOption
                        [java.nio.file.StandardCopyOption/REPLACE_EXISTING])))
        (.. path toFile deleteOnExit)
        (str (.toAbsolutePath path))))))

(defn- candidate-paths
  "libsqltree locations in precedence order; nil entries (e.g. no
  embedded copy) are dropped by the consumer."
  []
  (let [name (System/mapLibraryName "sqltree")]
    (concat
      (some-> (System/getProperty "parseq.lib") vector)
      (for [dir (.split (str (System/getProperty "java.library.path") "") File/pathSeparator)
            :when (not (.isEmpty ^String (str dir)))]
        (File. ^String dir name))
      [(embedded-path)])))

(defonce ^:private library
  (delay
    (or (first
          (keep #(let [s (some-> % str not-empty)
                       f (some-> ^String s File.)]
                   (when (.exists ^File f) s))
                (candidate-paths)))
        (throw (ex-info "libsqltree not found; build it (clojure -T:build native) and set -Djava.library.path or -Dparseq.lib, or use a parseq jar that carries natives for this platform"
                        {:searched (vec (remove nil? (candidate-paths)))})))))

(defonce ^:private lookup
  (delay
    ;; The arena outlives every downcall symbol reference; leaking one
    ;; shared arena per process is the intended lifetime.
    (let [arena (Arena/ofShared)
          lookup (SymbolLookup/libraryLookup ^String @library arena)]
      (fn [^String name]
        (or (-> ^SymbolLookup lookup (.find name) (.orElse nil))
            (throw (ex-info (str "libsqltree symbol missing: " name)
                            {:library @library :symbol name})))))))

(defn- downcall ^MethodHandle [^String name ret arg-layouts]
  (let [desc (if ret
               (FunctionDescriptor/of ret (into-array ValueLayout arg-layouts))
               (FunctionDescriptor/ofVoid (into-array ValueLayout arg-layouts)))
        no-opts (make-array Linker$Option 0)]
    (.downcallHandle ^Linker (Linker/nativeLinker)
                     ^MemorySegment ((deref lookup) name)
                     ^FunctionDescriptor desc
                     no-opts)))

(defonce ^:private handles
  (delay
    (let [ptr ValueLayout/ADDRESS]
      {:version     (downcall "sqt_version" ptr [])
       :open        (downcall "sqt_open" ptr [ptr])
       :close       (downcall "sqt_close" nil [ptr])
       :parse       (downcall "sqt_parse" ptr [ptr ptr])
       :load-schema (downcall "sqt_load_schema" ptr [ptr ptr])
       :free        (downcall "sqt_free" nil [ptr])})))

(defn- seg->string
  "Copy a NUL-terminated C string out of a native segment; nil for
  NULL. Returned pointer segments have zero extent, so reinterpret to
  a readable size first."
  [^MemorySegment seg]
  (when (and seg (not= MemorySegment/NULL seg))
    (.getString (.reinterpret seg Long/MAX_VALUE) 0)))

(defn- ->cstr [^Arena arena ^String s]
  (.allocateFrom arena s))

(defn- invoke-free
  "Free a SQLite-owned result buffer."
  [^MemorySegment seg]
  (when (and seg (not= MemorySegment/NULL seg))
    (.invokeWithArguments ^MethodHandle (:free @handles) (list seg))))

(deftype PanamaNative []
  native/Sqltree
  (native-open [_ path]
    (let [arena (Arena/ofShared)]
      (try
        (let [res (.invokeWithArguments ^MethodHandle (:open @handles)
                                        (list (->cstr arena (str (or path ":memory:")))))]
          (if (or (nil? res) (= MemorySegment/NULL res))
            (do (.close arena)
                (throw (ex-info "sqt_open failed" {:path path})))
            {:arena arena :handle res}))
        (catch Throwable t
          (.close arena)
          (throw t)))))
  (native-close [_ {:keys [arena handle]}]
    (try
      (when handle
        (.invokeWithArguments ^MethodHandle (:close @handles) (list handle)))
      (finally
        (when arena (.close ^Arena arena))))
    nil)
  (native-parse [_ {:keys [handle]} sql]
    (with-open [arena (Arena/ofConfined)]
      (let [res (.invokeWithArguments ^MethodHandle (:parse @handles)
                                      (list handle (->cstr arena (str sql))))
            text (seg->string res)]
        (invoke-free res)
        (or text (throw (ex-info "sqt_parse returned NULL (out of memory?)" {:sql sql}))))))
  (native-load-schema [_ {:keys [handle]} ddl]
    (with-open [arena (Arena/ofConfined)]
      (let [res (.invokeWithArguments ^MethodHandle (:load-schema @handles)
                                      (list handle (->cstr arena (str ddl))))
            text (seg->string res)]
        (invoke-free res)
        text)))
  (native-version [_]
    (with-open [arena (Arena/ofConfined)]
      (let [res (.invokeWithArguments ^MethodHandle (:version @handles) [])]
        (or (seg->string res) "unknown")))))
