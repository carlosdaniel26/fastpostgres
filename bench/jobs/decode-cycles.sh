#!/usr/bin/env bash
# Runs inside the dev image: the justfile's `decode-cycles` recipe hands this to
# the container as its command.
#
# The row decode alone, on a fixed shape, in one process: the extension's own
# rdtsc report, printed at exit on stderr -
#
#   read_rows: 159.7 cycles/row over 300000 rows (decode 79.3, frame+desc+list 80.4)
#
# That is the ten-minute price of a change: run it, change the decode, run it.
#
#   decode-cycles.sh [--shape bench|mixed] [rows] [batches]
#
# The number only exists if the decode ran: an unsupported kind takes the
# columnar frame, which has no cycle accounting, so this script fails loudly
# when the line is missing instead of printing nothing.
set -euo pipefail

ghz=$(lscpu 2>/dev/null | awk -F: '/CPU max MHz/ {printf "%.1f", $2 / 1000; exit}')
cpu=$(awk -F': ' '/^model name/ {print $2; exit}' /proc/cpuinfo)

echo "### DECODE-CYCLES"
echo "cpu: ${cpu:-unknown}"
echo "ghz: ${ghz:-unknown}"
echo "args: ${*:-<driver defaults>}"
echo "### END DECODE-CYCLES"

report=$(mktemp)
trap 'rm -f "$report"' EXIT

status=0
python3 /src/bench/internals/decode_driver.py "$@" 2>"$report" || status=$?

echo "--- the extension's report ---"
cat "$report"

if [ "$status" -ne 0 ]; then
    echo "decode-cycles: the driver exited $status" >&2
    exit "$status"
fi

if ! grep -q '^read_rows:' "$report"; then
    echo "decode-cycles: no 'read_rows:' line: the decode did not run" >&2
    echo "  (a columnar frame has no cycle accounting; check the shape's kinds)" >&2
    exit 1
fi
