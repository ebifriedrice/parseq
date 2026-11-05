/*
** sqltree_all.c -- the single translation unit for libsqltree.
**
** The serializer in sqltree.c reads SQLite's internal structures
** (Select, Expr, SrcList, Parse, ...) which are defined privately
** inside the amalgamation. Rather than duplicating struct layouts or
** defeating SQLITE_PRIVATE across compilation units, both files are
** compiled together here, exactly like the amalgamation itself
** intends for embedders.
**
** vendor/sqlite3.c is the upstream amalgamation with parsecov.patch
** applied (see native/vendor/README.md); sqlite3.h resolves next to it.
*/
#include "../vendor/sqlite3.c"
#include "sqltree.c"
