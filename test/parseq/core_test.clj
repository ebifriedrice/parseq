(ns parseq.core-test
  (:require [clojure.test :refer [deftest is testing]]
            [parseq.core :as sut]))

(deftest placeholder
  (testing "namespace loads"
    (is (some? (find-ns 'parseq.core)))))
