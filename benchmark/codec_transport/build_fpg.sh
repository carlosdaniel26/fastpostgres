#!/usr/bin/env bash
# build_fpg.sh — build the FPG codec cell inside the dev image.
#
#   docker run --rm -v "$PWD":/src -w /src/benchmark/codec_transport \
#       fastpostgres-dev bash build_fpg.sh
#
# Sources: fpg_codec.cpp + fpg_bench.cpp (both include pg-extension/fastpostgres_shm.h).
# Artifacts land only under benchmark/codec_transport/build/.
#
# The dev image's entrypoint runs its command as the unprivileged `postgres`
# user, so build/ must be writable by that user; if it does not exist yet,
# create it on the host first (it is git-ignored):
#   mkdir -p benchmark/codec_transport/build && chmod 777 benchmark/codec_transport/build
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
build="$here/build"

if ! mkdir -p "$build" 2>/dev/null || [ ! -w "$build" ]; then
    echo "build_fpg.sh: cannot write $build as $(id -un) (uid $(id -u))." >&2
    echo "  The image runs commands as uid 999 (postgres); make the (git-ignored)" >&2
    echo "  build directory writable by it, e.g. on the host:" >&2
    echo "    mkdir -p benchmark/codec_transport/build && chmod 777 benchmark/codec_transport/build" >&2
    exit 1
fi

CXX="${CXX:-g++}"
# -O3 per SPEC.md: pyarrow ships optimized (SIMD) kernels and the PostgreSQL
# extension itself builds -O3, so the codec is compiled at the same level.
# -fno-strict-aliasing backs the may_alias typed reads of the payload.
flags=(-O3 -std=c++17 -fno-strict-aliasing -Wall -Wextra -I "$root/pg-extension" -I "$root/pg-extension/src")

echo "build_fpg.sh: $CXX ${flags[*]}" >&2
"$CXX" "${flags[@]}" -o "$build/fpg_bench" "$here/fpg_bench.cpp" "$here/fpg_codec.cpp"
echo "build_fpg.sh: wrote $build/fpg_bench" >&2
