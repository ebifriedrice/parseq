cmake_minimum_required(VERSION 3.20)
project(parseq_sqltree LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Ship the DLL without a CRT dependency: users of the jar should not
# need the VC++ redistributable installed (what sqlite-jdbc does too).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------------------------------------------------------------------------
# vendored sqlite: vendor/sqlite3.c is upstream 3.54.1 with
# patch/parsecov.patch applied; vendor/sqlite3.h is pristine upstream.
# Nothing is downloaded and nothing is patched at build time.
# ---------------------------------------------------------------------------
set(SQLITE_C ${CMAKE_CURRENT_SOURCE_DIR}/vendor/sqlite3.c)

# ---------------------------------------------------------------------------
# TK_ code -> name table, generated from the vendored amalgamation so it
# can never drift from the token numbering of the sqlite we build against
# ---------------------------------------------------------------------------
set(OPNAMES_H ${CMAKE_CURRENT_BINARY_DIR}/sqltree_opnames.h)
add_custom_command(
  OUTPUT ${OPNAMES_H}
  COMMAND ${CMAKE_COMMAND}
    -DIN=${SQLITE_C} -DOUT=${OPNAMES_H}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_opnames.cmake
  DEPENDS ${SQLITE_C}
  COMMENT "generating sqltree_opnames.h")

# ---------------------------------------------------------------------------
# libsqltree: JNI entry points + plain C ABI (sqt_*) for Panama
# ---------------------------------------------------------------------------
find_package(JNI REQUIRED COMPONENTS JVM)  # just to locate jni.h via include path
find_package(Java COMPONENTS Development REQUIRED)

# vendor/sqlite3.c is compiled by inclusion inside sqltree_all.c; it is
# a dependency, not its own object. The opnames header is generated, so
# it must force a recompile of the TU explicitly (Ninja does not track
# includes of files it did not compile).
add_library(sqltree SHARED src/sqltree_all.c)
add_custom_target(opnames_h DEPENDS ${OPNAMES_H})
add_dependencies(sqltree opnames_h)
set_source_files_properties(src/sqltree_all.c PROPERTIES
  OBJECT_DEPENDS "${SQLITE_C};${OPNAMES_H}")

target_include_directories(sqltree PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}
  ${JNI_INCLUDE_DIRS})

target_compile_definitions(sqltree PRIVATE
  SQT_ENABLE_JNI=1
  SQLITE_ENABLE_COLUMN_METADATA=0)

if(MSVC)
  target_compile_options(sqltree PRIVATE /W4)
else()
  target_compile_options(sqltree PRIVATE -fvisibility=hidden -Wall -Wextra)
endif()

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
