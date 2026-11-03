#!/usr/bin/env bash
# Runs inside the profile image (valgrind): the justfile's `decode-ir` recipe
# hands this to the container as its command.
#
# The decode's dynamic instruction count and its miss/mispredict profile, on the
# same fixed shape `just decode-cycles` uses. Two runs:
#
#   1. plain             -> the rdtsc line: real TSC cycles/row
#   2. under callgrind, with collection toggled on for fp_decode_tuples only
#      (--collect-atstart=no --toggle-collect=*fp_decode_tuples*) -> Ir, D1/LL
#      misses and branch mispredicts for exactly the scope the rdtsc decode
#      figure covers, with CPython's startup out of the picture
#
# Scoping it that way is what makes the IPC honest: same function, same run
# shape, both sides of the division from this script. The number settles the
# 334-static-instruction bound in NOTES.md, which counted the loop's
# own address span and so missed everything the row executes outside it (the
# kind cases behind the jump table, and the callees).
#
# callgrind is a simulation, not the PMU (which is also why it works where perf
# cannot, in the container), and TSC ticks are not core cycles on a boosting
# part: the IPC printed here is at the TSC clock - read the ghz above before
# comparing it with anything else.
#
#   decode-ir.sh [--shape bench|mixed] [rows] [batches]
#     defaults: 10000 3 (valgrind is ~50-100x slower than decode-cycles)
set -euo pipefail

orig_args="$*"
shape_args=()
rows=10000
batches=3
seen=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --shape)
            if [ "$#" -lt 2 ]; then
                echo "decode-ir: --shape needs a value" >&2
                exit 1
            fi
            shape_args=(--shape "$2")
            shift 2
            ;;
        *)
            case "$seen" in
                0) rows=$1 ;;
                1) batches=$1 ;;
                *) echo "decode-ir: too many arguments" >&2; exit 1 ;;
            esac
            seen=$((seen + 1))
            shift
            ;;
    esac
done
if [ "$batches" -lt 1 ]; then
    echo "decode-ir: batches must be at least 1" >&2
    exit 1
fi

ghz=$(lscpu 2>/dev/null | awk -F: '/CPU max MHz/ {printf "%.1f", $2 / 1000; exit}')
cpu=$(awk -F': ' '/^model name/ {print $2; exit}' /proc/cpuinfo)

echo "### DECODE-IR"
echo "cpu: ${cpu:-unknown}"
echo "ghz: ${ghz:-unknown}"
echo "valgrind: $(valgrind --version)"
echo "args: ${orig_args:-<defaults: $rows $batches>}"
echo "### END DECODE-IR"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

echo "--- 1/2 plain run (the rdtsc numbers) ---"
python3 /src/bench/internals/decode_driver.py ${shape_args[@]+"${shape_args[@]}"} "$rows" "$batches" \
    2>"$work/plain.err" | tee "$work/plain.out"
cat "$work/plain.err"

read_rows_line=$(grep -m1 '^read_rows:' "$work/plain.err" || true)
if [ -z "$read_rows_line" ]; then
    echo "decode-ir: no 'read_rows:' line from the plain run: the decode did not run" >&2
    exit 1
fi
cycles_total=$(printf '%s\n' "$read_rows_line" | awk '{print $2}')
cycles_decode=$(printf '%s\n' "$read_rows_line" | sed -n 's/.*(decode \([0-9.]*\).*/\1/p')

field() { awk -v want="$1" '{for (i = 1; i <= NF; i++) if ($i == want) {print $(i + 1); exit}}' "$2"; }
warmup=$(field warmup: "$work/plain.out")
decoded=$(awk '/^decode_driver:/ {for (i = 1; i <= NF; i++) if ($i ~ /^decoded=/) {sub("decoded=", "", $i); print $i; exit}}' "$work/plain.out")
if [ -z "$warmup" ] || [ -z "$decoded" ]; then
    echo "decode-ir: could not read warmup/decoded from the driver's output" >&2
    exit 1
fi
rows_decoded=$((warmup + decoded))

echo "--- 2/2 callgrind, collection toggled on for the decode itself ---"
valgrind --tool=callgrind --cache-sim=yes --branch-sim=yes \
    --collect-atstart=no --toggle-collect='*fp_decode_tuples*' \
    --callgrind-out-file="$work/cg.decode" \
    python3 /src/bench/internals/decode_driver.py ${shape_args[@]+"${shape_args[@]}"} "$rows" "$batches" \
    >"$work/cg.out" 2>"$work/cg.err"

echo "--- valgrind totals (the decode and its callees: the toggle's scope) ---"
grep -E "^==[0-9]+== (I   refs|I1  misses|LLi misses|D   refs|D1  misses|LLd misses|Branches|Mispredicts|Mispred rate)" \
    "$work/cg.err" || true

ir=$(sed -n 's/^==[0-9]*== I *refs: *\([0-9,]*\).*/\1/p' "$work/cg.err" | tr -d ',')
if [ -z "$ir" ] || [ "$ir" -eq 0 ]; then
    echo "decode-ir: the toggle collected nothing: fp_decode_tuples was not called?" >&2
    exit 1
fi

if [ -s "$work/cg.decode" ]; then
    callgrind_annotate --threshold=90 "$work/cg.decode" >"$work/annotate.txt"
    awk '/^Ir/ {p = 1} p' "$work/annotate.txt" >"$work/top.txt"
    echo "--- callgrind_annotate, self costs inside the decode (context) ---"
    head -20 "$work/top.txt"
    echo "--- how callgrind_annotate splits it by file attribution (context) ---"
    callgrind_annotate --inclusive=yes --auto=no --threshold=99 "$work/cg.decode" >"$work/incl.txt"
    grep -E 'fp_decode_tuples|decode_tuple' "$work/incl.txt" || true
fi

echo "--- the decode's dynamic cost ---"
python3 - "$work/cg.err" "$rows_decoded" "$cycles_total" "$cycles_decode" <<'PY'
import re
import sys

path, rows, cycles_total, cycles_decode = sys.argv[1:5]
rows = int(rows)
ct, cd = float(cycles_total), float(cycles_decode)

with open(path, encoding="utf-8", errors="replace") as fh:
    text = fh.read()


def total(label):
    m = re.search(r"^==\d+==\s*" + label + r"\s*:\s*([\d,]+)", text, re.M)
    return int(m.group(1).replace(",", "")) if m else 0


def triple(label):
    """valgrind's '(n cond + m ind)' breakdown, total first."""
    m = re.search(r"^==\d+==\s*" + label + r"\s*:\s*([\d,]+)\s*\(\s*([\d,]+) cond\s*\+\s*([\d,]+) ind\)", text, re.M)
    if not m:
        return total(label), 0, 0
    return tuple(int(v.replace(",", "")) for v in m.groups())


ir = total(r"I\s+refs")
d1, d1rd, d1wr = triple(r"D1\s+misses")
lld = total(r"LLd\s+misses")
br = triple(r"Branches")
mp = triple(r"Mispredicts")
ir_row = ir / rows

print(f"decode-ir: rows decoded          = {rows}  (batches x rows + the warmup batch)")
print(f"decode-ir: decode Ir/row         = {ir_row:.1f}  (callgrind, fp_decode_tuples and its callees)")
print(f"decode-ir: decode cycles/row     = {cd:.1f}  (rdtsc; whole read_rows {ct:.1f})")
print(f"decode-ir: implied IPC at the TSC clock = {ir_row / cd:.2f}")
print(f"decode-ir: per row: D1 misses {d1 / rows:.1f} of {ir_row:.1f} Ir ({1000 * d1 / ir:.1f} per 1000), "
      f"LLd {lld / rows:.1f}, branches {br[0] / rows:.1f} "
      f"({br[1] / rows:.1f} cond, {br[2] / rows:.1f} ind), "
      f"mispredicts {mp[0] / rows:.2f} ({mp[1] / rows:.2f} cond, {mp[2] / rows:.2f} ind)")
PY
