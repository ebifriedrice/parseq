#!/bin/sh
# Download the pinned SQLite amalgamation and verify its SHA3-256.
#
# The amalgamation is build input, not source of truth: it is not
# committed. parsecov.patch in native/patch/ carries every modification
# we make, so the tree stays reviewable against the pristine upstream
# file this script fetches.
set -eu

VERSION=3540100
HUMAN=3.54.1
URL="https://sqlite.org/2026/sqlite-amalgamation-${VERSION}.zip"
SHA3_256="92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30"

dir=$(cd "$(dirname "$0")" && pwd)
dest="$dir/sqlite-amalgamation-${VERSION}"

if [ -f "$dest/sqlite3.c" ] && [ -f "$dest/sqlite3.h" ]; then
  exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "fetching ${URL}" >&2
curl -fsSL -o "$tmp/amalg.zip" "$URL"

got=$(openssl dgst -sha3-256 "$tmp/amalg.zip" | awk '{print $NF}')
if [ "$got" != "$SHA3_256" ]; then
  echo "sha3-256 mismatch: expected $SHA3_256, got $got" >&2
  exit 1
fi

unzip -oq "$tmp/amalg.zip" -d "$tmp"
mkdir -p "$dest"
cp "$tmp/sqlite-amalgamation-${VERSION}/sqlite3.c" \
   "$tmp/sqlite-amalgamation-${VERSION}/sqlite3.h" \
   "$dest/"
echo "sqlite ${HUMAN} ready in $dest" >&2
