# Vendored SQLite sources

- Version: 3.54.1
- Upstream: https://sqlite.org/2026/sqlite-amalgamation-3540100.zip
- SHA3-256 of the upstream zip:
  92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30

`sqlite3.h` is byte-for-byte upstream.

`sqlite3.c` is upstream **with parsecov.patch applied** (see
`../patch/parsecov.patch`, 29 hunks). The patch is the reviewable
record of every change parseq makes to SQLite; it is kept in the tree
so `git log` on this file never has to stand in for reading the delta.

To re-derive `sqlite3.c` from upstream:

    curl -fsSLO https://sqlite.org/2026/sqlite-amalgamation-3540100.zip
    unzip sqlite-amalgamation-3540100.zip
    patch sqlite-amalgamation-3540100/sqlite3.c ../patch/parsecov.patch

To bump the SQLite version: fetch the new amalgamation, check whether
the patch applies cleanly (the hook anchors are exact source lines and
will refuse loudly if upstream moved them), re-anchor as needed, and
commit the new vendored pair together with the updated patch.
