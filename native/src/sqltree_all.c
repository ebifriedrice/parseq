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
** sqlite_patched.c is generated at build time by copying the pristine
** vendored amalgamation and applying native/patch/parsecov.patch.
*/
#include "sqlite_patched.c"
#include "sqltree.c"
