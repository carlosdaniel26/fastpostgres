#!/usr/bin/env bash
# arrow_transport_bench.sh — the ADBC + Arrow read with the ring's engine as a
# second factor: the same query once through fastpostgres over shm, once over
# regipc, and (measured once: it does not depend on the engine) ADBC + Arrow.
#
# The committed Arrow record (bench/results/fastpostgres-vs-arrow.log) and
# the transport record (bench/results/transport-e2e.log) are not comparable
# across engines: the former measured one engine (shm) on this benchmark's schema
# and players, the latter measured both engines on the workload suite's schema.
# This harness measures all three cells in one run. Per size, one fresh process
# per engine runs bench/rivals/adbc-arrow/sqlalchemy_bench.py: FASTPOSTGRES_TRANSPORT
# is read at import and a ring's engine is fixed at creation, so a process
# cannot serve both engines. The ADBC players ride inside those runs; they are
# engine-independent, so they are measured once per size and recorded once.
#
# The regipc cell works in a container because the device node travels:
# `docker run --device /dev/regipc`. Without it the cell cannot be measured, and
# this harness fails with the remedy instead of silently recording one engine.
# The image's entrypoint runs the client as postgres, which can open the 0666
# node.
#
# Per (size, engine) cell the harness also runs its own probe — one read of the
# benchmark's table through that engine — and scans the container's /dev/shm for
# the control rings the cell's player processes left. Every ring must carry the
# engine that was asked for (its header is read here, not taken from a player),
# its requests must have been answered, and the probe read must have been served
# by the ring (conn.shm_reads 1/1). The probe digests the table (count and the
# numeric columns' sums, the generated data's own closed form), so the record
# shows the same rows in every cell. A ring that fell back to libpq, a missing
# ring, or a different digest fails the run.
#
# seconds and p50_ms are derived from the script's printed median rate: one read
# per sample (the 4-process row is 4 parallel reads), so seconds = rows / rate
# and p50_ms = seconds * 1000. The script prints no per-player times or CPU.
#
# Usage: bench/harness/arrow_transport_bench.sh [--quick] [--keep]
#                                 [--engines=shm,regipc] [--sizes=N,N]
#
#   --quick    a small size (100k), for a smoke run
#   --keep     leave the container and the raw cell outputs for inspection
#   --engines  which ring engines to measure (default both; a single engine is
#              an explicit request and the record says so)
#   --sizes    row counts, comma-separated (default 1000000,10000000)
#
# stdout is the record: ### ENV, each cell's own tables, one table per size with
# the three families side by side, the digest/rings verdict, and ### RESULTS:
# one line of JSON with one record per (size, engine, player) — size, engine,
# player, unit, value, seconds, p50_ms, ring_transport, served_by_shm,
# digest_ok, valid. The adbc records say in the JSON that they are measured
# once, in the first engine's process.
#
# Run: bench/harness/arrow_transport_bench.sh > bench/results/arrow-transport.log
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
image=${IMAGE:-fastpostgres-dev}
shm=${SHM:-3g}
container=fp-arrow-transport
player=/src/bench/rivals/adbc-arrow/sqlalchemy_bench.py
probe_dir=/tmp/fp-arrow-transport

quick=0
keep=0
engines="shm regipc"
sizes=""

usage() {
  sed -n '/^# Usage:/,/^#   --sizes/p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
  case "$1" in
    --quick) quick=1 ;;
    --keep) keep=1 ;;
    --engines=*) engines=${1#--engines=} ;;
    --engines)
      if [ $# -lt 2 ]; then
        echo "arrow_transport: --engines needs a value (e.g. shm,regipc)" >&2
        exit 2
      fi
      shift
      engines=$1
      ;;
    --sizes=*) sizes=${1#--sizes=} ;;
    --sizes)
      if [ $# -lt 2 ]; then
        echo "arrow_transport: --sizes needs a value (e.g. 1000000,10000000)" >&2
        exit 2
      fi
      shift
      sizes=$1
      ;;
    -h | --help) usage; exit 0 ;;
    *)
      echo "arrow_transport: unknown flag or argument '$1' (see --help)" >&2
      exit 2
      ;;
  esac
  shift
done

engines=${engines//,/ }
sizes=${sizes//,/ }
if [ "$quick" = 1 ]; then
  sizes=${sizes:-100000}
else
  sizes=${sizes:-1000000 10000000}
fi
read -r -a row_sizes <<<"$sizes"
if [ ${#row_sizes[@]} -eq 0 ]; then
  echo "arrow_transport: no sizes to measure" >&2
  exit 2
fi
for n in "${row_sizes[@]}"; do
  case "$n" in
    *[!0-9]* | '' | 0)
      echo "arrow_transport: --sizes takes positive integers, not '$n'" >&2
      exit 2
      ;;
  esac
done
if [ -z "$engines" ]; then
  echo "arrow_transport: --engines is empty; expected shm and/or regipc" >&2
  exit 2
fi
for name in $engines; do
  # shellcheck disable=SC2194 # a literal list of the engines this harness knows
  case " shm regipc " in
    *" $name "*) ;;
    *)
      echo "arrow_transport: unknown engine '$name' (expected shm and/or regipc)" >&2
      exit 2
      ;;
  esac
done

want_regipc=0
for name in $engines; do [ "$name" = regipc ] && want_regipc=1; done

# The remedy, not a silent single-engine record. The module is a human step
# (insmod); the device is the host's to pass to the container.
if [ "$want_regipc" = 1 ] && [ ! -e /dev/regipc ]; then
  echo "arrow_transport: /dev/regipc is missing: the regipc module is not loaded." >&2
  echo "  sudo insmod $root/extension/transport/regipc/module/regipc.ko mode=0666" >&2
  echo "  (the container gets the node with --device; the host must have it first)" >&2
  exit 1
fi

work=$(mktemp -d)
# shellcheck disable=SC2329 # invoked by the EXIT trap
cleanup() {
  if [ "$keep" = 1 ]; then
    echo "arrow_transport: kept the container '$container' and the raw cell outputs in $work" >&2
    return 0
  fi
  docker rm -f "$container" >/dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT

if ! docker build -q --target dev -t "$image" -f "$root/docker/Dockerfile" "$root" \
  >/dev/null 2>"$work/build.err"; then
  sed 's/^/  /' "$work/build.err" >&2
  echo "arrow_transport: the image build failed; fix it before measuring" >&2
  exit 1
fi
image_id=$(docker image inspect --format '{{.Id}}' "$image" | cut -c1-19)
gitrev=$(git -C "$root" describe --always --dirty 2>/dev/null || echo unknown)

docker rm -f "$container" >/dev/null 2>&1 || true
device_args=()
if [ "$want_regipc" = 1 ]; then device_args=(--device /dev/regipc); fi
docker run -d --name "$container" --shm-size="$shm" ${device_args[@]+"${device_args[@]}"} \
  -e "SHAPE=${SHAPE:-}" \
  -e "FASTPOSTGRES_OBJ_ARENA=${OBJ_ARENA:-}" \
  "$image" sleep infinity >/dev/null
for _ in $(seq 120); do docker exec "$container" pg_isready -h /tmp -q && break; sleep 1; done
if ! docker exec "$container" pg_isready -h /tmp -q; then
  echo "arrow_transport: the server did not come up in the container; its log:" >&2
  docker logs "$container" --tail 40 >&2 || true
  exit 1
fi

# The device as the container sees it, and as the user the reads will run as. A
# container started without --device has no node; one whose node is not 0666
# cannot be opened by postgres. Either way: name the remedy and stop.
regipc_container="not measured (regipc is not among the engines)"
if [ "$want_regipc" = 1 ]; then
  if ! docker exec "$container" test -c /dev/regipc; then
    echo "arrow_transport: the container cannot see /dev/regipc, so the regipc cell cannot run." >&2
    echo "  the host must pass the node: docker run --device /dev/regipc ... (this harness does)" >&2
    echo "  and the module must be loaded on the host:" \
      "sudo insmod $root/extension/transport/regipc/module/regipc.ko mode=0666" >&2
    exit 1
  fi
  if ! docker exec -u postgres "$container" python3 -c \
    'import os; os.close(os.open("/dev/regipc", os.O_RDWR))' 2>"$work/regipc.err"; then
    echo "arrow_transport: postgres inside the container cannot open /dev/regipc:" >&2
    sed 's/^/  /' "$work/regipc.err" >&2
    echo "  the module was probably loaded with a restrictive mode; reload it:" >&2
    echo "  sudo rmmod regipc && sudo insmod $root/extension/transport/regipc/module/regipc.ko mode=0666" >&2
    exit 1
  fi
  regipc_container=$(docker exec "$container" stat -c '%F mode 0%a, %t:%T' /dev/regipc)
fi

cpu=$(awk -F': ' '/^model name/ {print $2; exit}' /proc/cpuinfo)
mem=$(awk '/MemTotal:/ {printf "%.0f GiB", $2 / 1048576}' /proc/meminfo)
ghz=$(lscpu 2>/dev/null | awk -F: '/CPU max MHz/ {printf "%.1f", $2 / 1000; exit}')
cores=$(nproc)
kernel=$(uname -r)
container_cores=$(docker exec "$container" nproc)
pg_version=$(docker exec "$container" psql --version | awk '{print $3}')
py_versions=$(docker exec "$container" python3 -c \
  'import sys, importlib.metadata as m; print(sys.version.split()[0], m.version("sqlalchemy"), m.version("pyarrow"), m.version("adbc_driver_postgresql"))')
read -r py_version sqlalchemy_version pyarrow_version adbc_version <<<"$py_versions"
shape=${SHAPE:-bench}
regipc_mode="absent"
regipc_version=""
regipc_srcversion=""
regipc_params=""
if [ -e /dev/regipc ]; then
  regipc_mode=$(stat -c '0%a' /dev/regipc 2>/dev/null || echo unknown)
  regipc_version=$(cat /sys/module/regipc/version 2>/dev/null || echo unknown)
  regipc_srcversion=$(cat /sys/module/regipc/srcversion 2>/dev/null || echo unknown)
  for p in /sys/module/regipc/parameters/*; do
    [ -e "$p" ] || continue
    regipc_params="${regipc_params:+$regipc_params,}${p##*/}=$(<"$p")"
  done
fi

# The two loop scripts, written into the container once: the ring scan (what a
# cell's processes left in /dev/shm) and the probe (one read through the engine,
# with its digest and its ring) are part of this harness, so both live here.
cat >"$work/ring_scan.py" <<'PY'
"""Scan the container's /dev/shm for the control rings a cell's processes left.

Prints one JSON object with the rings' headers as the harness proof needs them:
magic, version, transport (1 shm, 2 regipc), client_pid, req_seq/resp_seq and
worker_ready. Run inside the container as postgres, which owns them.
"""
import json
import mmap
import os
import struct
import sys

RING_HEADER = "<8sIIIIQQQ"  # ring.h: magic, version, ready, pid, transport, data_bytes, req_seq, resp_seq


def main():
    rings = []
    for name in sorted(os.listdir("/dev/shm")):
        if not name.startswith("fp_dbapi_") or "_seg" in name:
            continue
        path = os.path.join("/dev/shm", name)
        try:
            with open(path, "rb") as fh, mmap.mmap(fh.fileno(), 0, prot=mmap.PROT_READ) as view:
                (magic, version, ready, pid, transport, data_bytes, req, resp) = struct.unpack_from(
                    RING_HEADER, view, 0)
        except OSError as exc:
            rings.append({"path": path, "error": str(exc)})
            continue
        rings.append({
            "path": path,
            "magic": magic.decode("latin-1"),
            "version": version,
            "worker_ready": ready,
            "client_pid": pid,
            "transport": transport,
            "data_bytes": data_bytes,
            "req_seq": req,
            "resp_seq": resp,
            "bytes": os.stat(path).st_size,
        })
    print(json.dumps({"pid": os.getpid(), "rings": rings}, separators=(",", ":")))


if __name__ == "__main__":
    sys.exit(main())
PY
cat >"$work/probe.py" <<'PY'
"""One read of the benchmark's table through one engine, for a cell's proof.

Run inside the container as postgres, once per (size, engine) after the
benchmark's process has exited:

    FASTPOSTGRES_TRANSPORT=regipc python3 probe.py 1000000

It reads the benchmark's table (bench/rivals/adbc-arrow/sqlalchemy_bench.py's rows),
digests it (count and the numeric columns' sums, the generated data's closed
form) and reports the client's own evidence: how many reads the ring served
(conn.shm_reads) and the ring file this process created, with the header read
from /dev/shm - transport 1 (shm) or 2 (regipc), data_bytes, client_pid. The
players' numbers stay the benchmark's.
"""
import json
import mmap
import os
import struct
import sys

from fastpostgres import dbapi

DIGEST_SQL = "SELECT count(*), sum(id), sum(price), sum(stock), sum(active::int) FROM fp_bench_rows"
RING_HEADER = "<8sIIIIQQQ"
TRANSPORT_ID = {"shm": 1, "regipc": 2}


def ring_files(pid):
    stem = f"fp_dbapi_{pid}_"
    return [os.path.join("/dev/shm", name)
            for name in sorted(os.listdir("/dev/shm"))
            if name.startswith(stem) and "_seg" not in name]


def ring_headers(pid):
    out = []
    for path in ring_files(pid):
        with open(path, "rb") as fh, mmap.mmap(fh.fileno(), 0, prot=mmap.PROT_READ) as view:
            (magic, version, ready, client_pid, transport, data_bytes, req, resp) = struct.unpack_from(
                RING_HEADER, view, 0)
        out.append({"path": path, "magic": magic.decode("latin-1"), "version": version,
                    "worker_ready": ready, "client_pid": client_pid, "transport": transport,
                    "data_bytes": data_bytes, "req_seq": req, "resp_seq": resp})
    return out


def expected(n):
    """The closed form of the generated data (bench/rivals/adbc-arrow/sqlalchemy_bench.py)."""
    cycles, rem = divmod(n, 500)
    return [n, n * (n + 1) // 2, n * (n + 1) / 4,
            cycles * sum(range(500)) + sum(range(1, rem + 1)), n // 2]


def same_digest(left, right, tolerance=1e-9):
    """Integers exactly, floats within a relative tolerance (lib/harness.py)."""
    for a, b in zip(left, right):
        if isinstance(a, float) or isinstance(b, float):
            scale = abs(a) + abs(b)
            if scale and abs(a - b) > tolerance * scale:
                return False
        elif a != b:
            return False
    return True


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 100000
    engine = dbapi.transport()
    conn = dbapi.connect("host=/tmp dbname=postgres user=postgres")
    try:
        cur = conn.cursor()
        cur.execute(DIGEST_SQL)
        row = cur.fetchone()
        digest = [int(row[i]) if i != 2 else float(row[i]) for i in range(5)]
        cur.close()
        served = conn.shm_reads
    finally:
        conn.close()
    rings = ring_headers(os.getpid())
    want = expected(n)
    problems = []
    if served != 1:
        problems.append(f"the read fell back to libpq: shm_reads={served} for 1 read")
    if len(rings) != 1:
        problems.append(f"expected one ring for pid {os.getpid()}, found {[r['path'] for r in rings]}")
    for ring in rings:
        if ring["magic"] != "FPRING04" or ring["version"] != 4:
            problems.append(f"{ring['path']}: magic {ring['magic']!r} version {ring['version']}, not FPRING04 v4")
        if ring["transport"] != TRANSPORT_ID[engine]:
            problems.append(f"{ring['path']}: transport {ring['transport']}, "
                            f"{engine} is {TRANSPORT_ID[engine]}")
    if not same_digest(digest, want):
        problems.append(f"digest {digest} != the generated data's {want}")
    print(json.dumps({
        "n": n, "engine": engine, "sql": DIGEST_SQL, "digest": digest, "expected": want,
        "reads": 1, "served_by_shm": served, "rings": rings,
        "digest_ok": same_digest(digest, want), "valid": not problems, "problems": problems,
    }, separators=(",", ":")))
    if problems:
        for problem in problems:
            print(f"probe: {problem}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
PY
docker exec -i "$container" sh -c "mkdir -p $probe_dir" >/dev/null
docker exec -i "$container" sh -c "cat > $probe_dir/ring_scan.py" <"$work/ring_scan.py"
docker exec -i "$container" sh -c "cat > $probe_dir/probe.py" <"$work/probe.py"

echo "### ENV"
echo "cpu: ${cpu:-unknown}"
echo "mem: ${mem:-unknown}"
echo "ghz: ${ghz:-unknown}"
echo "cores: $cores (host nproc; the container sees $container_cores)"
echo "kernel: $kernel"
echo "postgres: $pg_version (in the container)"
echo "python: $py_version, sqlalchemy $sqlalchemy_version, pyarrow $pyarrow_version, adbc $adbc_version (in the container, where the players run)"
echo "fastpostgres image: $image $image_id"
echo "shm: $shm (/dev/shm in the container; the ring's data segments live there)"
echo "shape: $shape (SHAPE; the schema bench/rivals/adbc-arrow/sqlalchemy_bench.py builds)"
echo "regipc: /dev/regipc mode ${regipc_mode}; module version ${regipc_version:-unknown};" \
  "srcversion ${regipc_srcversion:-unknown}; params ${regipc_params:-none}"
echo "regipc in the container: $regipc_container"
echo "engines: $engines (ring transport ids: shm 1, regipc 2)"
echo "sizes: ${row_sizes[*]}"
echo "git: $gitrev"
echo "# One fresh process per (size, engine): FASTPOSTGRES_TRANSPORT is read at"
echo "# import and a ring's engine is fixed at creation, so a process cannot serve"
echo "# both engines. The adbc players do not depend on it: they are measured once"
echo "# per size (in the first engine's process) and recorded once."
echo "# The table per size puts the three families side by side; every fastpostgres"
echo "# record also carries the cell's ring evidence - the rings the cell left in"
echo "# /dev/shm all name the engine, and the harness's own probe read was served by"
echo "# the ring (served_by_shm 1/1) with the same digest in every cell."
echo "### END ENV"
echo

failed=0
for size in "${row_sizes[@]}"; do
  for engine in $engines; do
    stem="$work/cell_${engine}_${size}"
    docker exec "$container" sh -c 'rm -f /dev/shm/fp_*' || true
    echo "# $engine / $size rows: one fresh process, FASTPOSTGRES_TRANSPORT=$engine"
    rc=0
    docker exec -u postgres -e "FASTPOSTGRES_TRANSPORT=$engine" "$container" \
      python3 "$player" "$size" >"$stem.out" 2>"$stem.err" || rc=$?
    printf '%s\n' "$rc" >"$stem.rc"
    sed -e '/^$/d' -e 's/^/  /' "$stem.out" || true
    if [ "$rc" != 0 ]; then
      tail -20 "$stem.err" | sed 's/^/  ! /' || true
      echo "  FAILED $engine/$size: the benchmark exited $rc"
      failed=1
    fi
    scan_rc=0
    docker exec -i -u postgres "$container" python3 "$probe_dir/ring_scan.py" \
      >"$stem.rings.json" 2>"$stem.rings.err" || scan_rc=$?
    printf '%s\n' "$scan_rc" >"$stem.rings.rc"
    probe_rc=0
    docker exec -u postgres -e "FASTPOSTGRES_TRANSPORT=$engine" "$container" \
      python3 "$probe_dir/probe.py" "$size" >"$stem.probe.json" 2>"$stem.probe.err" || probe_rc=$?
    printf '%s\n' "$probe_rc" >"$stem.probe.rc"
    if [ "$scan_rc" != 0 ] || [ "$probe_rc" != 0 ]; then
      sed 's/^/  ! /' "$stem.rings.err" "$stem.probe.err" 2>/dev/null || true
      echo "  FAILED $engine/$size: the ring scan exited $scan_rc, the ring probe exited $probe_rc"
      failed=1
    fi
  done
done

export BENCH_WORK="$work" BENCH_SIZES="${row_sizes[*]}" BENCH_ENGINES="$engines"
export BENCH_SHAPE="$shape" BENCH_GIT="$gitrev" BENCH_FAILED="$failed"
export BENCH_CPU="$cpu" BENCH_MEM="$mem" BENCH_GHZ="$ghz" BENCH_CORES="$cores"
export BENCH_KERNEL="$kernel" BENCH_CONTAINER_CORES="$container_cores"
export BENCH_POSTGRES="$pg_version" BENCH_PYTHON="$py_version" BENCH_SQLALCHEMY="$sqlalchemy_version"
export BENCH_PYARROW="$pyarrow_version" BENCH_ADBC="$adbc_version"
export BENCH_IMAGE="$image $image_id" BENCH_SHM="$shm"
export BENCH_REGIPC_MODE="$regipc_mode" BENCH_REGIPC_VERSION="$regipc_version"
export BENCH_REGIPC_SRCVERSION="$regipc_srcversion" BENCH_REGIPC_PARAMS="$regipc_params"
export BENCH_REGIPC_CONTAINER="$regipc_container"

# The tables and the one-line record. Everything below is aggregation over what
# the cells already reported; nothing is timed twice.
python3 <<'PY'
import json
import os
import re
import sys

work = os.environ["BENCH_WORK"]
sizes = [int(v) for v in os.environ["BENCH_SIZES"].split()]
engines = os.environ["BENCH_ENGINES"].split()
EXPECTED = {"shm": 1, "regipc": 2}

# player key, the label the script's PATHS table prints, rows delivered per read
PLAYERS = (
    ("dbapi", "fastpostgres, DBAPI", 1),
    ("stream", "fastpostgres, stream()+sum", 1),
    ("core", "fastpostgres, core interface", 1),
    ("sqla", "fastpostgres, SQLAlchemy CORE", 1),
    ("par", "fastpostgres, 4 processes", 4),
    ("arrow_sum", "adbc+arrow, arrow+sum", 1),
    ("fetchall", "adbc+arrow, fetchall()", 1),
    ("to_pylist", "adbc+arrow, table+to_pylist", 1),
)
FPG = ("dbapi", "stream", "core", "sqla", "par")
ADBC = ("arrow_sum", "fetchall", "to_pylist")
MULT = {key: mult for key, _label, mult in PLAYERS}
PRETTY = {"dbapi": "DBAPI", "stream": "stream()+sum", "core": "core interface",
          "sqla": "SQLAlchemy CORE", "par": "4 processes", "arrow_sum": "arrow+sum",
          "fetchall": "fetchall()", "to_pylist": "table+to_pylist"}
# A PATHS row: load, the player's label, the rate, an optional delta.
ROW = re.compile(r"^\s*(\d+)([KM])\s+((?:fastpostgres|adbc\+arrow), .+?)\s+([\d.]+)M(\s+[+-]\d+%)?\s*$")


def human(value):
    for unit, scale in (("G", 1e9), ("M", 1e6), ("k", 1e3)):
        if value >= scale:
            return f"{value / scale:.2f}{unit}".rstrip("0").rstrip(".")
    return f"{value:.0f}"


def rate_cell(value):
    return human(value) if value is not None else "--"


def ratio_cell(left, right):
    if left is None or right in (None, 0):
        return "--"
    return f"{left / right:.2f}x"


def same_digest(left, right, tolerance=1e-6):
    """Integers exactly, floats within a relative tolerance (lib/harness.py)."""
    for a, b in zip(left, right):
        if isinstance(a, float) or isinstance(b, float):
            scale = abs(a) + abs(b)
            if scale and abs(a - b) > tolerance * scale:
                return False
        elif a != b:
            return False
    return True


def read_json(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError) as exc:
        return {"error": f"{path}: {exc}"}


def read_rc(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return int(fh.read().strip())
    except (OSError, ValueError):
        return None


def parse_cell(stem, size):
    """One (engine, size) cell: the script's PATHS rates plus its own checks."""
    cell = {"rates": {}, "problems": [], "raw": ""}
    try:
        with open(stem + ".out", encoding="utf-8") as fh:
            cell["raw"] = fh.read()
    except OSError:
        cell["problems"].append("the benchmark produced no stdout")
    if "MISMATCH" in cell["raw"]:
        cell["problems"].append("the script printed MISMATCH (a section's players returned different row counts)")
    for line in cell["raw"].splitlines():
        match = ROW.match(line)
        if not match:
            continue
        label = match.group(3).strip()
        key = next((k for k, spec, _mult in PLAYERS if spec == label), None)
        if key is None:
            continue
        load = int(match.group(1)) * (1e6 if match.group(2) == "M" else 1e3)
        if load != size:
            cell["problems"].append(f"{label}: a row for {load} rows in the {size} cell")
        # the rate column is always printed in millions of rows/s (f"{rate:.2f}M")
        cell["rates"].setdefault(key, []).append(float(match.group(4)) * 1e6)
    for key, label, _mult in PLAYERS:
        if len(cell["rates"].get(key, [])) != 1:
            cell["problems"].append(
                f"the PATHS table has {len(cell['rates'].get(key, []))} rows for '{label}', expected 1")
    return cell


def cell_evidence(engine, size):
    """The cell's transport proof: the rings it left and the harness's probe."""
    stem = os.path.join(work, f"cell_{engine}_{size}")
    evidence = {"rings": [], "probe": None, "problems": [], "rings_ok": False}
    problem = evidence["problems"].append
    evidence["rc"] = read_rc(stem + ".rc")
    if evidence["rc"] != 0:
        problem(f"the benchmark exited {evidence['rc']}")
    scan_rc, probe_rc = read_rc(stem + ".rings.rc"), read_rc(stem + ".probe.rc")
    if scan_rc != 0:
        problem(f"the ring scan exited {scan_rc}")
    if probe_rc != 0:
        problem(f"the ring probe exited {probe_rc}")
    scan = read_json(stem + ".rings.json")
    if "error" in scan:
        problem(f"the ring scan said: {scan['error']}")
    else:
        evidence["rings"] = scan.get("rings", [])
    probe = read_json(stem + ".probe.json")
    if "error" in probe:
        problem(f"the ring probe said: {probe['error']}")
    else:
        evidence["probe"] = probe
    want = EXPECTED[engine]
    evidence["rings_ok"] = bool(evidence["rings"])
    if not evidence["rings"]:
        problem("no control ring was left in /dev/shm: the engine did not serve the reads")
    for ring in evidence["rings"]:
        if ring.get("magic") != "FPRING04" or ring.get("version") != 4:
            evidence["rings_ok"] = False
            problem(f"{ring.get('path')}: magic {ring.get('magic')!r} version {ring.get('version')}, "
                    "not FPRING04 v4")
        elif ring.get("transport") != want:
            evidence["rings_ok"] = False
            problem(f"{ring.get('path')}: transport {ring.get('transport')}, {engine} is {want}")
        elif not ring.get("req_seq") or ring.get("req_seq") != ring.get("resp_seq"):
            evidence["rings_ok"] = False
            problem(f"{ring.get('path')}: req_seq {ring.get('req_seq')} != resp_seq {ring.get('resp_seq')} "
                    "(requests were not all answered)")
    evidence["served_rings"] = sum(1 for ring in evidence["rings"]
                                   if ring.get("transport") == want and ring.get("req_seq") == ring.get("resp_seq"))
    if evidence["probe"]:
        probe_ev = evidence["probe"]
        if probe_ev.get("n") != size:
            problem(f"the probe read {probe_ev.get('n')} rows, not {size}")
        if probe_ev.get("engine") != engine:
            problem(f"the probe says engine {probe_ev.get('engine')!r}, not {engine!r}")
        if not probe_ev.get("valid"):
            problem("the probe marked itself invalid: " + "; ".join(probe_ev.get("problems") or []))
        if probe_ev.get("served_by_shm") != probe_ev.get("reads", 1):
            problem(f"the probe's ring served {probe_ev.get('served_by_shm')} of "
                    f"{probe_ev.get('reads', 1)} reads")
    evidence["served_by_shm"] = (evidence["probe"] or {}).get("served_by_shm")
    return evidence


cells = {}
for size in sizes:
    for engine in engines:
        stem = os.path.join(work, f"cell_{engine}_{size}")
        cell = parse_cell(stem, size)
        cell["evidence"] = cell_evidence(engine, size)
        cells[(size, engine)] = cell

# The digest verdict: the probe's own check against the generated data's closed
# form, and the same digest across the engines that were measured for a size.
digest_ok = {}
for size in sizes:
    reference = None
    for engine in engines:
        probe = cells[(size, engine)]["evidence"]["probe"]
        if probe:
            reference = probe
            break
    ok = reference is not None and bool(reference.get("digest_ok"))
    for engine in engines:
        probe = cells[(size, engine)]["evidence"]["probe"]
        if probe and reference and not same_digest(probe["digest"], reference["digest"]):
            ok = False
            cells[(size, engine)]["evidence"]["problems"].append(
                f"the digest {probe['digest']} differs from {reference['digest']}")
    digest_ok[size] = ok

# The records: every fastpostgres player per engine, and the adbc players once
# per size (from the first engine's process, which measured them anyway).
records = []
for size in sizes:
    for engine in engines:
        cell = cells[(size, engine)]
        evidence = cell["evidence"]
        problems = list(cell["problems"]) + list(evidence["problems"])
        valid = not problems
        if not valid:
            for problem in problems:
                print(f"  INVALID {engine}/{size}: {problem}")
        for key in FPG:
            rates = cell["rates"].get(key) or []
            value = rates[0] if rates else None
            seconds = MULT[key] * size / value if value else None
            records.append({
                "size": size,
                "engine": engine,
                "player": key,
                "unit": "rows/s",
                "value": round(value, 3) if value else None,
                "seconds": round(seconds, 6) if seconds else None,
                "p50_ms": round(seconds * 1000, 3) if seconds else None,
                "ring_transport": EXPECTED[engine] if evidence["rings_ok"] else None,
                "served_by_shm": evidence["served_by_shm"] if evidence["rings_ok"] else None,
                "digest_ok": digest_ok[size],
                "valid": valid,
            })
    source = cells[(size, engines[0])]
    adbc_valid = (not source["problems"] and source["evidence"]["rc"] == 0 and digest_ok[size])
    for key in ADBC:
        if key not in source["rates"]:
            continue
        value = source["rates"][key][0]
        seconds = MULT[key] * size / value
        records.append({
            "size": size,
            "engine": "adbc",
            "player": key,
            "unit": "rows/s",
            "value": round(value, 3),
            "seconds": round(seconds, 6),
            "p50_ms": round(seconds * 1000, 3),
            "ring_transport": None,
            "served_by_shm": None,
            "digest_ok": digest_ok[size],
            "valid": adbc_valid,
        })

# ---- the tables -----------------------------------------------------------

for size in sizes:
    print()
    print(f"--- {size:,} rows: the three families, delivered rows/s (one read of every row per player) ---")
    print(f"  {'player':<16s} {'fpg shm':>12s} {'fpg regipc':>12s} {'adbc+arrow':>12s}"
          f" {'regipc/shm':>12s} {'shm/arrow':>11s} {'regipc/arrow':>13s}")
    arrow_rate = (cells[(size, engines[0])]["rates"].get("arrow_sum") or [None])[0]
    for key in FPG:
        shm = (cells[(size, "shm")]["rates"].get(key) or [None])[0] if "shm" in engines else None
        regipc = (cells[(size, "regipc")]["rates"].get(key) or [None])[0] if "regipc" in engines else None
        print(f"  {PRETTY[key]:<16s} {rate_cell(shm):>12s} {rate_cell(regipc):>12s} {'--':>12s}"
              f" {ratio_cell(regipc, shm):>12s} {ratio_cell(shm, arrow_rate):>11s}"
              f" {ratio_cell(regipc, arrow_rate):>13s}")
    for key in ADBC:
        rate = (cells[(size, engines[0])]["rates"].get(key) or [None])[0]
        print(f"  {PRETTY[key]:<16s} {'--':>12s} {'--':>12s} {rate_cell(rate):>12s}"
              f" {'--':>12s} {'--':>11s} {'--':>13s}")
    print("  # ratio columns: regipc/shm is the same player under the two engines;"
          " shm/arrow and regipc/arrow are against adbc+arrow, arrow+sum (1.00 = Arrow)")
    reference = next((cells[(size, e)]["evidence"]["probe"] for e in engines
                      if cells[(size, e)]["evidence"]["probe"]), None)
    if reference:
        digest = reference["digest"]
        print(f"  # digest (the harness's own probe read, once per engine, over the same table):"
              f" count={digest[0]:,} sum(id)={digest[1]:,} sum(price)={digest[2]:,}"
              f" sum(stock)={digest[3]:,} sum(active)={digest[4]:,}"
              f" -- {'identical in every cell' if digest_ok[size] else 'MISMATCH'}")
    rings = []
    for engine in engines:
        ev = cells[(size, engine)]["evidence"]
        probe_ev = ev["probe"] or {}
        rings.append(f"{engine}: {len(ev['rings'])} control ring(s), {ev['served_rings']} of them"
                     f" answered requests with transport {EXPECTED[engine]},"
                     f" probe served_by_shm {probe_ev.get('served_by_shm')}/{probe_ev.get('reads', 1)}")
    print("  # rings: " + "; ".join(rings))
    counts_ok = all(not cells[(size, engine)]["problems"] and
                    cells[(size, engine)]["evidence"]["rc"] == 0 for engine in engines)
    print(f"  # every player returned exactly {size:,} rows: "
          f"{'ok' if counts_ok else 'see the INVALID lines'}")

print()
first_engine = engines[0]
adbc_note = (f"adbc+arrow players do not depend on FASTPOSTGRES_TRANSPORT: measured once per size "
             f"(in the {first_engine} process) and recorded once; the other engine's run measured them "
             f"again, and those duplicates stay in the raw cells, out of the record.")
print(f"# {adbc_note}")
if len(engines) == 1:
    print(f"# WARNING this record has one engine ({' '.join(engines)}): this run does not compare the two, by request")

problems = []
if os.environ.get("BENCH_FAILED") not in ("0", None):
    problems.append("a benchmark, ring scan or probe process exited non-zero")
for size in sizes:
    if not digest_ok[size]:
        problems.append(f"{size}: the digest is not the same in every cell")
    for engine in engines:
        cell = cells[(size, engine)]
        if cell["problems"] or cell["evidence"]["problems"]:
            problems.append(f"{size}/{engine}")

machine = {
    "cpu": os.environ["BENCH_CPU"],
    "mem": os.environ["BENCH_MEM"],
    "ghz": os.environ["BENCH_GHZ"],
    "cores": int(os.environ["BENCH_CORES"]),
    "container_cores": int(os.environ["BENCH_CONTAINER_CORES"]),
    "kernel": os.environ["BENCH_KERNEL"],
    "postgres": os.environ["BENCH_POSTGRES"],
    "python": os.environ["BENCH_PYTHON"],
    "sqlalchemy": os.environ["BENCH_SQLALCHEMY"],
    "pyarrow": os.environ["BENCH_PYARROW"],
    "adbc": os.environ["BENCH_ADBC"],
    "image": os.environ["BENCH_IMAGE"],
    "shm": os.environ["BENCH_SHM"],
    "shape": os.environ["BENCH_SHAPE"],
    "sizes": sizes,
    "engines": engines,
    "git": os.environ["BENCH_GIT"],
    "regipc": {
        "device": "/dev/regipc",
        "mode": os.environ["BENCH_REGIPC_MODE"],
        "module_version": os.environ["BENCH_REGIPC_VERSION"],
        "srcversion": os.environ["BENCH_REGIPC_SRCVERSION"],
        "params": os.environ["BENCH_REGIPC_PARAMS"],
        "in_container": os.environ["BENCH_REGIPC_CONTAINER"],
    },
}
print("### RESULTS")
print(json.dumps({
    "machine": machine,
    "records": records,
    "cells": [
        {
            "size": size,
            "engine": engine,
            "digest": (cells[(size, engine)]["evidence"]["probe"] or {}).get("digest"),
            "rings": len(cells[(size, engine)]["evidence"]["rings"]),
            "rings_served": cells[(size, engine)]["evidence"]["served_rings"],
            "ring_transport": EXPECTED[engine] if cells[(size, engine)]["evidence"]["rings_ok"] else None,
            "probe_served_by_shm": cells[(size, engine)]["evidence"]["served_by_shm"],
            "probe_reads": 1,
            "digest_ok": digest_ok[size],
            "problems": cells[(size, engine)]["problems"] + cells[(size, engine)]["evidence"]["problems"],
        }
        for size in sizes for engine in engines
    ],
    "adbc_note": adbc_note,
}, separators=(",", ":")))
print("### END RESULTS")
if problems:
    print(f"arrow_transport: {len(problems)} problem(s): " + "; ".join(str(p) for p in problems),
          file=sys.stderr)
    sys.exit(1)
PY

if [ "$failed" != 0 ]; then
  echo "arrow_transport: a cell process exited non-zero; see the FAILED lines above" >&2
  exit 1
fi
