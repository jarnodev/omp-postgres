#!/bin/sh
# Builds a release archive in the same layout as other open.mp components:
#   dist/omp-postgres-<version>-linux.tar.gz
#     components/Postgres.so
#     qawno/include/omp_postgres.inc
#     LICENSE, README.md
set -eu

here="$(cd "$(dirname "$0")" && pwd)"
version="$(sed -n 's/^project(omp-postgres .*VERSION \([0-9.]*\)).*/\1/p' "$here/CMakeLists.txt")"
name="omp-postgres-$version-linux"

CONFIG=Release "$here/build.sh"

stage="$here/build/package/$name"
rm -rf "$here/build/package"
mkdir -p "$stage/components" "$stage/qawno/include" "$here/dist"
cp "$here/build/Postgres.so" "$stage/components/"
cp "$here/include/omp_postgres.inc" "$stage/qawno/include/"
cp "$here/LICENSE" "$here/README.md" "$stage/"

tar czf "$here/dist/$name.tar.gz" -C "$stage" .
echo "Created dist/$name.tar.gz"
