cmake_minimum_required(VERSION 3.20)
project(parseq_sqltree LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------------------------------------------------------------------------
# vendored amalgamation: fetch if missing, copy, apply parsecov.patch
# ---------------------------------------------------------------------------
set(SQLITE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/sqlite-amalgamation-3540100)
if(NOT EXISTS ${SQLITE_DIR}/sqlite3.c)
  message(STATUS "fetching sqlite amalgamation")
  execute_process(
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/vendor/fetch.sh
    RESULT_VARIABLE fetch_rc)
  if(NOT fetch_rc EQUAL 0)
    message(FATAL_ERROR "fetch.sh failed (${fetch_rc})")
  endif()
endif()

set(PATCHED_C ${CMAKE_CURRENT_BINARY_DIR}/sqlite_patched.c)
add_custom_command(
  OUTPUT ${PATCHED_C}
  COMMAND ${CMAKE_COMMAND} -E copy ${SQLITE_DIR}/sqlite3.c ${PATCHED_C}
  COMMAND patch --silent ${PATCHED_C} ${CMAKE_CURRENT_SOURCE_DIR}/patch/parsecov.patch
  COMMAND ${CMAKE_COMMAND} -E copy ${SQLITE_DIR}/sqlite3.h ${CMAKE_CURRENT_BINARY_DIR}/sqlite3.h
  DEPENDS ${SQLITE_DIR}/sqlite3.c ${CMAKE_CURRENT_SOURCE_DIR}/patch/parsecov.patch
  COMMENT "applying parsecov.patch to sqlite3.c")

# ---------------------------------------------------------------------------
# TK_ code -> name table, generated from the patched amalgamation so it
# can never drift from the token numbering of the sqlite we build against
# ---------------------------------------------------------------------------
set(OPNAMES_H ${CMAKE_CURRENT_BINARY_DIR}/sqltree_opnames.h)
add_custom_command(
  OUTPUT ${OPNAMES_H}
  COMMAND ${CMAKE_COMMAND}
    -DIN=${PATCHED_C} -DOUT=${OPNAMES_H}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_opnames.cmake
  DEPENDS ${PATCHED_C}
  COMMENT "generating sqltree_opnames.h")

# ---------------------------------------------------------------------------
# libsqltree: JNI entry points + plain C ABI (sqt_*) for Panama
# ---------------------------------------------------------------------------
find_package(JNI REQUIRED COMPONENTS JVM)  # just to locate jni.h via include path
find_package(Java COMPONENTS Development REQUIRED)

# sqlite_patched.c is compiled by inclusion inside sqltree_all.c; it is
# a dependency, not its own object. Both generated inputs must force a
# recompile of the TU.
add_library(sqltree SHARED src/sqltree_all.c)
add_dependencies(sqltree opnames_h patched_c)
add_custom_target(patched_c DEPENDS ${PATCHED_C})
set_source_files_properties(src/sqltree_all.c PROPERTIES
  OBJECT_DEPENDS "${PATCHED_C};${OPNAMES_H}")

# phony target so the opnames custom command always runs before compilation
add_custom_target(opnames_h DEPENDS ${OPNAMES_H})

target_include_directories(sqltree PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}
  ${JNI_INCLUDE_DIRS})

target_compile_definitions(sqltree PRIVATE
  SQT_ENABLE_JNI=1
  SQLITE_ENABLE_COLUMN_METADATA=0)

target_compile_options(sqltree PRIVATE -fvisibility=hidden -Wall -Wextra)

set_target_properties(sqltree PROPERTIES
  OUTPUT_NAME sqltree
  VERSION 0
  SOVERSION 0)

# ---------------------------------------------------------------------------
# smoke test: exercise the C ABI from plain C before any JVM is involved
# ---------------------------------------------------------------------------
add_executable(sqltree_smoke test/sqltree_smoke.c)
target_link_libraries(sqltree_smoke PRIVATE sqltree)

# convenience: ctest
enable_testing()
add_test(NAME smoke COMMAND sqltree_smoke)
