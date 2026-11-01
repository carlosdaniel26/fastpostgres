#!/usr/bin/env bash
# build_transport.sh — compiles the transport cell of the codec/transport
# isolation experiment into benchmark/codec_transport/build/ (git-ignored).
#
# Run inside the dev image, from benchmark/codec_transport/:
#   docker run --rm -v /home/carlos/Projects/fastpostgres:/src -w /src/benchmark/codec_transport \
#     fastpostgres-dev bash build_transport.sh
#
# Nothing outside build/ is written.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$here/build"

# The dev image runs as `postgres` (uid 999) while the bind mount belongs to the
# host user, so build/ has to exist and be writable before this script can write
# into it. Say so instead of dying with a bare "Permission denied".
if ! mkdir -p "$out" 2>/dev/null; then
    echo "build_transport.sh: cannot create $out inside the bind mount." >&2
    echo "  On the host, once:  mkdir -p benchmark/codec_transport/build && chmod 777 benchmark/codec_transport/build" >&2
    echo "  (benchmark/codec_transport/build/ is git-ignored; it is where SPEC.md puts all artifacts.)" >&2
    exit 1
fi
if [ ! -w "$out" ]; then
    echo "build_transport.sh: $out exists but is not writable by $(id -un) (uid $(id -u))." >&2
    echo "  On the host:  chmod 777 benchmark/codec_transport/build" >&2
    exit 1
fi

src="$here/transport_bench.cpp"
bin="$out/transport_bench"

# -pthread: sem_t/sem_post/sem_timedwait (glibc 2.34+ has them in libc, but the
# flag is what the ring's build uses too and costs nothing).
set -x
g++ -O3 -g -std=c++17 -Wall -Wextra -Wno-unused-parameter -pthread -o "$bin" "$src"
set +x

echo "built $bin"
