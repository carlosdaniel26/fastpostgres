#!/usr/bin/env bash
# The workload suite: realistic scenarios over a commerce schema, fastpostgres
# against psycopg 3 - the plain driver the same application ran on before.
#
# One container, a throwaway PostgreSQL with the extension: the schema is created
# fresh at the requested scale, every scenario runs each player in a process of
# its own (several for the concurrency and mixed ones), the digests are compared
# across players, and stdout is the log:
#
#   just bench > docs/fastpostgres-vs-psycopg.log
#
# Usage: tools/workloads_bench.sh [scenario...] [--quick] [--scale=N] [--reps=N]
#                                 [--duration=S] [--clients=N] [--keep]
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
image=${IMAGE:-fastpostgres-dev}
shm=${SHM:-3g}
# The transfer buffer this suite runs with. An application's reads are one batch
# each, so 64 MiB is already generous; the ring's 1 GiB default would reserve 2 GiB
# of address space per client (eight of them in the concurrency scenario) for
# batches of a few MB. Passed to the players and printed in the log.
segment_mib=${SEGMENT_MIB:-64}
container=fp-workloads

keep=0
args=()
for arg in "$@"; do
  case "$arg" in
    --keep) keep=1 ;;
    *) args+=("$arg") ;;
  esac
done

cleanup() {
  if [ "$keep" = 1 ]; then return 0; fi
  docker rm -f "$container" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker build -q --target dev -t "$image" -f "$root/image/Dockerfile" "$root" >/dev/null 2>&1
docker rm -f "$container" >/dev/null 2>&1 || true
docker run -d --name "$container" --shm-size="$shm" \
  -e "BENCH_GIT_REV=$(git -C "$root" describe --always --dirty 2>/dev/null || echo unknown)" \
  -e "BENCH_IMAGE=$image $(docker image inspect --format '{{.Id}}' "$image" | cut -c1-19)" \
  -e "BENCH_SHM=$shm" \
  -e "FASTPOSTGRES_SEGMENT_BYTES=$((segment_mib * 1024 * 1024))" \
  -e "BENCH_SEGMENT=$segment_mib MiB x2" \
  "$image" sleep infinity >/dev/null
for _ in $(seq 60); do docker exec "$container" pg_isready -h /tmp -q && break; sleep 1; done

# as postgres: the ring worker runs as the server's user, and the players share
# /dev/shm with it
docker exec -u postgres "$container" python3 /src/benchmark/workloads/runner.py \
  ${args[@]+"${args[@]}"}
