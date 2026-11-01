#!/usr/bin/env bash
# Runs inside the profile image (llvm): the justfile's `decode-mca` recipe hands
# this to the container as its command.
#
# The real decode loop, modeled offline by llvm-mca: ports, throughput and the
# bottleneck resource. With `just decode-ir` ("how many instructions per row")
# this is the other half - "what those instructions cost on the core".
#
# The default region is the outermost loop of fp_decode_tuples: the row loop of
# the decode, one iteration per row with the six kind cases inlined (~313 static
# instructions against the 334 the notes counted). loop-asm.py takes it out of
# the built extension and prints what it picked - addresses, instruction count,
# source lines, how many calls it holds - so the model can be audited.
#
#   decode-mca.sh [llvm-mca args ...]     e.g. --timeline -iterations=1000
#                 --no-asm                hide the extracted .s
#                 --list                  every candidate loop, no model
#                 --line N                model the loop containing codec.cpp:N
#
# MCA_CPU (default znver4) picks the modeled core.
set -euo pipefail

cpu=$(awk -F': ' '/^model name/ {print $2; exit}' /proc/cpuinfo)
orig_args="$*"

mca=""
for cand in llvm-mca llvm-mca-20 llvm-mca-19 llvm-mca-18 llvm-mca-17 /usr/lib/llvm-*/bin/llvm-mca; do
    if command -v "$cand" >/dev/null 2>&1; then
        mca=$(command -v "$cand")
        break
    fi
    if [ -x "$cand" ]; then
        mca=$cand
        break
    fi
done
if [ -z "$mca" ]; then
    echo "decode-mca: no llvm-mca in the image (the profile stage installs llvm)" >&2
    exit 1
fi

so=$(ls /src/python-extension/fastpostgres/_client*.so 2>/dev/null | head -1)
if [ -z "$so" ]; then
    echo "decode-mca: the client is not built in the image" >&2
    exit 1
fi

loop_args=()
mca_args=()
list_only=0
show_asm=1
while [ "$#" -gt 0 ]; do
    case "$1" in
        --list) list_only=1; shift ;;
        --no-asm) show_asm=0; shift ;;
        --line)
            if [ "$#" -lt 2 ]; then
                echo "decode-mca: --line needs a line number" >&2
                exit 1
            fi
            loop_args+=(--line "$2")
            shift 2
            ;;
        *) mca_args+=("$1"); shift ;;
    esac
done

mca_version=$("$mca" --version)

echo "### DECODE-MCA"
echo "cpu: ${cpu:-unknown}"
echo "llvm-mca: ${mca_version%%$'\n'*}"
echo "object: $so"
echo "sha256: $(sha256sum "$so" | cut -d' ' -f1)"
echo "args: ${orig_args:-<defaults>}"
echo "### END DECODE-MCA"

# loop-asm.py writes the region on stdout (the .s) and its statistics on stderr
if [ "$list_only" -eq 1 ]; then
    python3 /usr/local/bin/loop-asm.py --so "$so" --list ${loop_args[@]+"${loop_args[@]}"}
    exit 0
fi
python3 /usr/local/bin/loop-asm.py --so "$so" ${loop_args[@]+"${loop_args[@]}"} >/tmp/loop.s

if [ "$show_asm" -eq 1 ]; then
    echo "--- the region, as llvm-mca sees it ---"
    cat /tmp/loop.s
fi

echo "--- llvm-mca ($mca) ---"
"$mca" -mcpu="${MCA_CPU:-znver4}" -iterations=100 -bottleneck-analysis /tmp/loop.s \
    ${mca_args[@]+"${mca_args[@]}"}
