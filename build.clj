(ns build
  (:refer-clojure :exclude [test])
  (:require [clojure.java.io :as io]
            [clojure.tools.build.api :as b]
            [deps-deploy.deps-deploy :as dd]))

(def lib 'net.clojars.ebifriedrice/parseq)
(def version (or (System/getenv "PARSEQ_VERSION") "0.1.0-SNAPSHOT"))
(def class-dir "target/classes")

(defn native
  "Build libsqltree (and the C smoke test) with CMake + Ninja."
  [opts]
  (let [{:keys [exit]} (b/process {:command-args
                                   ["cmake" "-S" "native" "-B" "native/build"
                                    "-G" "Ninja"]
                                  :out :inherit})]
    (when-not (zero? exit) (throw (ex-info "cmake configure failed" {}))))
  (let [{:keys [exit]} (b/process {:command-args
                                   ["cmake" "--build" "native/build"]
                                  :out :inherit})]
    (when-not (zero? exit) (throw (ex-info "cmake build failed" {}))))
  opts)

(defn javac
  "Compile the JNI glue class parseq.native.Sqltree.

  release 8: the glue is plain JNI (loadLibrary + native methods), so
  keep it loadable by the old JVMs the JNI binding exists for; the
  FFM path never loads this class."
  [opts]
  (b/javac {:src-dirs ["java"]
            :class-dir class-dir
            :basis (b/create-basis {})
            :javac-opts ["--release" "8" "-Xlint:all,-options"]})
  opts)

(defn test "Run all the tests." [opts]
  (native nil)
  (javac nil)
  (let [basis    (b/create-basis {:aliases [:test]
                                  :extra-paths [class-dir "java"]})
        cmds     (b/java-command
                  {:basis      basis
                    :main      'clojure.main
                    :main-args ["-m" "cognitect.test-runner"]})
        {:keys [exit]} (b/process cmds)]
    (when-not (zero? exit) (throw (ex-info "Tests failed" {}))))
  opts)

(defn- pom-template [version]
  [[:description "FIXME: my new library."]
   [:url "https://github.com/ebifriedrice/parseq"]
   [:licenses
    [:license
     [:name "Eclipse Public License 2.0"]
     [:url "https://www.eclipse.org/legal/epl-2.0"]]]
   [:developers
    [:developer
     [:name "Ebi"]]]
   [:scm
    [:url "https://github.com/ebifriedrice/parseq"]
    [:connection "scm:git:https://github.com/ebifriedrice/parseq.git"]
    [:developerConnection "scm:git:ssh:git@github.com:ebifriedrice/parseq.git"]
    [:tag (str "v" version)]]])

(defn- stage-natives
  "Copy the prebuilt native libraries staged under natives/ (layout
  parseq/native/<os>/<arch>/<lib>, what the release workflow's
  per-arch jobs upload) into the class dir, so the jar carries them
  and the bindings can extract them at runtime. A no-op when the
  staging dir is absent, so local jars keep building without it."
  []
  (when (.exists (io/file "natives"))
    (b/copy-dir {:src-dirs ["natives"] :target-dir class-dir})))

(defn- jar-opts [opts]
  (assoc opts
          :lib lib   :version version
          :jar-file  (format "target/%s-%s.jar" lib version)
          :basis     (b/create-basis {})
          :class-dir class-dir
          :target    "target"
          :src-dirs  ["src"]
          :pom-data  (pom-template version)))

(defn ci "Run the CI pipeline of tests (and build the JAR)." [opts]
  (test opts)
  (b/delete {:path "target"})
  (let [opts (jar-opts opts)]
    (println "\nWriting pom.xml...")
    (b/write-pom opts)
    (println "\nCopying source...")
    (b/copy-dir {:src-dirs ["resources" "src"] :target-dir class-dir})
    (stage-natives)
    (println "\nBuilding JAR..." (:jar-file opts))
    (b/jar opts))
  opts)

(defn install "Install the JAR locally." [opts]
  (let [opts (jar-opts opts)]
    (stage-natives)
    (b/install opts))
  opts)

(defn deploy "Deploy the JAR to Clojars." [opts]
  (let [{:keys [jar-file] :as opts} (jar-opts opts)]
    (dd/deploy {:installer :remote :artifact (b/resolve-path jar-file)
                :pom-file (b/pom-path (select-keys opts [:lib :class-dir]))}))
  opts)
