#!/usr/bin/env bash
# The meters behind the design notes: the row decode's cycles, instructions and
# ports, the codec x transport matrix, and the host PMU's cache counters. These
# are not benchmarks of the product - they are the evidence `WHY-IT-WINS.md`
# and `CODEC-TRANSPORT.md` argue from - so they live behind one command and
# out of the README's benchmark table.
set -euo pipefail

target=${1:-}
[ $# -gt 0 ] && shift || true

usage() {
  cat <<'EOF'
internals: the meters behind the design notes

  decode-cycles    the row decode's TSC cycles/row, on a fixed shape
                   (NOTES.md)
  decode-ir        its dynamic instructions and miss profile, plus the IPC
                   (profile image; NOTES.md)
  decode-mca       the decode loop's ports and latency, modeled offline by llvm-mca
                   (profile image; NOTES.md)
  codec-transport  format cost vs transport cost, isolated (2x2 matrix)
                   (CODEC-TRANSPORT.md, bench/results/codec-transport.log)
  cache-stat       cache counters on the host's PMU: decodes per row, transports
                   per KiB (bench/results/cache-stat.log)
  client-scaling   the client itself: threads and asyncio, one shared-memory
                   partition per worker, against the serial baseline
                   (bench/results/client-scaling.log)

Arguments after the target go to that target's recipe.
EOF
}

case "$target" in
  decode-cycles)   just decode-cycles "$@" ;;
  decode-ir)       just decode-ir "$@" ;;
  decode-mca)      just decode-mca "$@" ;;
  codec-transport) just codec-transport "$@" ;;
  cache-stat)      just cache-stat "$@" ;;
  client-scaling)  just client-scaling "$@" ;;
  "" | list | help) usage ;;
  *)
    echo "internals: unknown target '$target'" >&2
    usage >&2
    exit 2
    ;;
esac
