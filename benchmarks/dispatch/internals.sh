#!/usr/bin/env bash
# The meters behind the design notes: the row decode's cycles, instructions and
# ports, the codec x transport matrix, and the host PMU's cache counters. These
# are not benchmarks of the product - they are the evidence `docs/WHY-IT-WINS.md`
# and `docs/CODEC-TRANSPORT.md` argue from - so they live behind one command and
# out of the README's benchmark table.
set -euo pipefail

target=${1:-}
[ $# -gt 0 ] && shift || true

usage() {
  cat <<'EOF'
internals: the meters behind the design notes

  decode-cycles    the row decode's TSC cycles/row, on a fixed shape
                   (docs/DECODE-NOTES.md)
  decode-ir        its dynamic instructions and miss profile, plus the IPC
                   (profile image; docs/DECODE-NOTES.md)
  decode-mca       the decode loop's ports and latency, modeled offline by llvm-mca
                   (profile image; docs/DECODE-NOTES.md)
  codec-transport  format cost vs transport cost, isolated (2x2 matrix)
                   (docs/CODEC-TRANSPORT.md, benchmarks/results/codec-transport.log)
  cache-stat       cache counters on the host's PMU: decodes per row, transports
                   per KiB (benchmarks/results/cache-stat.log)

Arguments after the target go to that target's recipe.
EOF
}

case "$target" in
  decode-cycles)   just decode-cycles "$@" ;;
  decode-ir)       just decode-ir "$@" ;;
  decode-mca)      just decode-mca "$@" ;;
  codec-transport) just codec-transport "$@" ;;
  cache-stat)      just cache-stat "$@" ;;
  "" | list | help) usage ;;
  *)
    echo "internals: unknown target '$target'" >&2
    usage >&2
    exit 2
    ;;
esac
