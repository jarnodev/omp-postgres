#!/bin/sh
# Builds the 32-bit Postgres component into build/Postgres.so.
#   ./build.sh                     Release build
#   CONFIG=Debug ./build.sh
#   ./build.sh /path/to/server     also install into an open.mp server
set -eu

here="$(cd "$(dirname "$0")" && pwd)"
config="${CONFIG:-Release}"

cmake -S "$here" -B "$here/build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$here/cmake/linux-i386.cmake" \
    -DCMAKE_BUILD_TYPE="$config"
cmake --build "$here/build" --parallel

if [ $# -ge 1 ]; then
    server="$(cd "$1" && pwd)"
    mkdir -p "$server/components" "$server/qawno/include"
    cp "$here/build/Postgres.so" "$server/components/Postgres.so"
    cp "$here/include/omp_postgres.inc" "$server/qawno/include/omp_postgres.inc"
    echo "Installed into $server"
fi
