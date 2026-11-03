#!/usr/bin/env bash
# transport_e2e_bench.sh — the record for the two transports, end to end.
#
# The isolated experiment (`just internals transport`, bench/jobs/
# transport.sh) measures the doorbell alone: one request descriptor in, one
# response out, no data segment. This harness measures the other half of the
# argument: a real read through PostgreSQL, the worker publishing real batches
# into /dev/shm, and the client decoding the rows — once with
# FASTPOSTGRES_TRANSPORT=shm (the ring's semaphores) and once with regipc (the
# kernel call through /dev/regipc).
#
#   bench/harness/transport_e2e_bench.sh > bench/results/transport-e2e.log
#
# Why the dev image can do it now: the regipc engine needs the module, and a
# container cannot load one, but the device node can be handed to it —
# `docker run --device /dev/regipc`. The image's entrypoint runs the client and
# the worker as postgres, so the 0666 node is openable by both. Without the
# device the regipc cell cannot be measured, and this harness fails with the
# remedy instead of silently recording one engine.
#
# One process per (engine, scenario, rep): FASTPOSTGRES_TRANSPORT is read at
# import and a ring's engine is fixed when the client creates it, so no process
# can measure both engines. The player (bench/internals/transport/
# e2e_player.py) ensures the scenario's rows exist, warms up, times the reads,
# folds their digest, and prints one line per read plus an `evidence` line: the
# timings, both processes' CPU (the client's from its own clock, the worker's
# from /proc/<pid>/stat at the pid the ring names), peak RSS, the digest, and the
# ring's own header — `transport` (1 shm, 2 regipc), `data_bytes`, the requests
# it posted, both sides' tally over the timed window (handoffs, semaphore posts,
# falls back to the semaphore, microseconds spent polling), and, for regipc, the
# module's `leaf.calls` from the page ring 0 wrote.
#
# A cell is valid only if `conn.shm_reads` advanced by exactly one per timed read
# (a read the ring did not serve falls back to libpq and does not count) AND the
# ring's header names the engine that was asked for. The harness reads that
# header a second time itself, from the container's /dev/shm, so the proof does
# not rest on the player's word. A digest that differs between engines, a
# fallback, or an unprovable engine makes the run fail; nothing is skipped
# quietly.
#
# Usage: bench/harness/transport_e2e_bench.sh [scenario...] [--quick] [--keep]
#                                 [--engines=shm,regipc] [--reps=N] [--reads=N]
#                                 [--scale=S] [--seed=N]
#
#   scenarios    scan (bulk: every row of one 1M-row scan, 1 read per rep),
#                lookup (primary-key lookups, one row each) and paged (filtered,
#                ordered pages of up to 100 rows); default all three
#   --quick      a tenth of the scale, 1 rep, 50 statements per rep (smoke run)
#   --keep       leave the container and the raw player output for inspection
#   --engines    which engines to measure (default both; one is an explicit
#                request and the record says so)
#   --reads      statements per rep for the statement scenarios (lookup, paged);
#                the scan always does one read of every row
#   --scale      the workload schema's scale (1 = 10k customers, 1M orders)
#   --seed       the parameter stream's seed (the same on both engines)
#
# stdout is the record: the ### TRANSPORT-E2E block (the machine, the image, the
# regipc device, the engines and scenarios, the caveats), the player's own lines
# for every rep, one table per scenario with both engines side by side, the
# digest verdict, and finally ### RESULTS: one line of JSON carrying the machine
# block and one record per (scenario, engine).
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
image=${IMAGE:-fastpostgres-dev}
shm=${SHM:-3g}
# The client's transfer buffer: two alternating data segments of this size per
# ring. 64 MiB holds a 100k-row batch of the scan's shape with room to spare (a
# scan of 1M rows arrives as 10 batches of 100k), and it is what the workload
# suite uses, so the two records read through the same buffer. Passed to the
# players and printed in the log.
segment_mib=${SEGMENT_MIB:-64}
# One ring per process: a sequential read needs one, and with exactly one the
# evidence can name the ring that served every timed read (the harness reads its
# header itself). A read that needed a second ring would fall back to libpq and
# fail the cell, which is the check working.
pool=1
container=fp-transport-e2e
player=/src/bench/internals/transport/e2e_player.py
known_scenarios="scan lookup paged"

quick=0
keep=0
engines="shm regipc"
reps=""
reads=""
scale=""
seed=7
scenarios=()

usage() {
  sed -n '/^# Usage:/,/^#   --seed/p' "$0" | sed 's/^# \{0,1\}//'
}

for arg in "$@"; do
  case "$arg" in
    --quick) quick=1 ;;
    --keep) keep=1 ;;
    --engines=*) engines=${arg#*=}; engines=${engines//,/ } ;;
    --reps=*) reps=${arg#*=} ;;
    --reads=*) reads=${arg#*=} ;;
    --scale=*) scale=${arg#*=} ;;
    --seed=*) seed=${arg#*=} ;;
    -h | --help) usage; exit 0 ;;
    --*)
      echo "transport_e2e: unknown flag '$arg' (see --help)" >&2
      exit 2
      ;;
    *) scenarios+=("$arg") ;;
  esac
done

for name in $engines; do
  # shellcheck disable=SC2194 # a literal list of the engines this harness knows
  case " shm regipc " in
    *" $name "*) ;;
    *)
      echo "transport_e2e: unknown engine '$name' (expected shm and/or regipc)" >&2
      exit 2
      ;;
  esac
done
for name in ${scenarios[@]+"${scenarios[@]}"}; do
  case " $known_scenarios " in
    *" $name "*) ;;
    *)
      echo "transport_e2e: unknown scenario '$name' (expected: $known_scenarios)" >&2
      exit 2
      ;;
  esac
done
if [ ${#scenarios[@]} -eq 0 ]; then scenarios=(scan lookup paged); fi

if [ "$quick" = 1 ]; then
  reps=${reps:-1}
  scale=${scale:-0.1}
  reads=${reads:-50}
else
  reps=${reps:-3}
  scale=${scale:-1}
fi
case "$reps" in
  *[!0-9]* | '' | 0)
    echo "transport_e2e: --reps must be a positive integer" >&2
    exit 2
    ;;
esac

want_regipc=0
for name in $engines; do [ "$name" = regipc ] && want_regipc=1; done

# The remedy, not a silent single-engine record. The module is a human step
# (insmod); the device is the host's to pass to the container.
if [ "$want_regipc" = 1 ] && [ ! -e /dev/regipc ]; then
  echo "transport_e2e: /dev/regipc is missing: the regipc module is not loaded." >&2
  echo "  sudo insmod $root/extension/transport/regipc/module/regipc.ko mode=0666" >&2
  echo "  (the container gets the node with --device; the host must have it first)" >&2
  exit 1
fi

work=$(mktemp -d)
# shellcheck disable=SC2329 # invoked by the EXIT trap
cleanup() {
  if [ "$keep" = 1 ]; then
    echo "transport_e2e: kept the container '$container' and the raw player output in $work" >&2
    return 0
  fi
  docker rm -f "$container" >/dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT

if ! docker build -q --target dev -t "$image" -f "$root/docker/Dockerfile" "$root" \
  >/dev/null 2>"$work/build.err"; then
  sed 's/^/  /' "$work/build.err" >&2
  echo "transport_e2e: the image build failed; fix it before measuring" >&2
  exit 1
fi
image_id=$(docker image inspect --format '{{.Id}}' "$image" | cut -c1-19)
gitrev=$(git -C "$root" describe --always --dirty 2>/dev/null || echo unknown)

docker rm -f "$container" >/dev/null 2>&1 || true
device_args=()
if [ "$want_regipc" = 1 ]; then device_args=(--device /dev/regipc); fi
docker run -d --name "$container" --shm-size="$shm" ${device_args[@]+"${device_args[@]}"} \
  -e "BENCH_GIT_REV=$gitrev" \
  -e "BENCH_IMAGE=$image $image_id" \
  -e "BENCH_SHM=$shm" \
  -e "BENCH_SEGMENT=$segment_mib MiB x2" \
  -e "FASTPOSTGRES_SEGMENT_BYTES=$((segment_mib * 1024 * 1024))" \
  -e "FASTPOSTGRES_SHM_POOL=$pool" \
  "$image" sleep infinity >/dev/null
for _ in $(seq 120); do docker exec "$container" pg_isready -h /tmp -q && break; sleep 1; done
if ! docker exec "$container" pg_isready -h /tmp -q; then
  echo "transport_e2e: the server did not come up in the container; its log:" >&2
  docker logs "$container" --tail 40 >&2 || true
  exit 1
fi

# The device as the container sees it, and as the user the reads will run as. A
# container started without --device has no node; one whose node is not 0666
# cannot be opened by postgres. Either way: name the remedy and stop.
regipc_container="not measured (regipc is not among the engines)"
if [ "$want_regipc" = 1 ]; then
  if ! docker exec "$container" test -c /dev/regipc; then
    echo "transport_e2e: the container cannot see /dev/regipc, so the regipc cell cannot run." >&2
    echo "  the host must pass the node: docker run --device /dev/regipc ... (this harness does)" >&2
    echo "  and the module must be loaded on the host:" \
      "sudo insmod $root/extension/transport/regipc/module/regipc.ko mode=0666" >&2
    exit 1
  fi
  if ! docker exec -u postgres "$container" python3 -c \
    'import os; os.close(os.open("/dev/regipc", os.O_RDWR))' 2>"$work/regipc.err"; then
    echo "transport_e2e: postgres inside the container cannot open /dev/regipc:" >&2
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
pg_version=$(docker exec "$container" psql --version | awk '{print $3}')
py_version=$(docker exec "$container" python3 -c 'import sys; print(sys.version.split()[0])')
psycopg_version=$(docker exec "$container" python3 -c 'import psycopg; print(psycopg.__version__.split()[0])')

echo "### TRANSPORT-E2E"
echo "cpu: ${cpu:-unknown}"
echo "mem: ${mem:-unknown}"
echo "ghz: ${ghz:-unknown}"
echo "cores: $cores (host nproc; the container sees $(docker exec "$container" nproc))"
echo "kernel: $kernel"
echo "postgres: $pg_version (in the container)"
echo "python: $py_version, psycopg $psycopg_version (in the container, where the players run)"
echo "fastpostgres image: $image $image_id"
echo "shm: $shm (/dev/shm in the container; the ring's data segments live there)"
echo "ring segment: $segment_mib MiB x2 per ring (FASTPOSTGRES_SEGMENT_BYTES); pool $pool (FASTPOSTGRES_SHM_POOL)"
echo "regipc: /dev/regipc mode ${regipc_mode}; module version ${regipc_version:-unknown};" \
  "srcversion ${regipc_srcversion:-unknown}; params ${regipc_params:-none}"
echo "regipc in the container: $regipc_container"
echo "engines: $engines (ring transport ids: shm 1, regipc 2)"
echo "scenarios: ${scenarios[*]}"
echo "reps: $reps (one process per engine x scenario x rep; the tables report the median over the reps)"
echo "scale: $scale; seed: $seed"
echo "git: $gitrev"
echo "# A read here is a real one end to end: PostgreSQL executes it, the ring's background"
echo "# worker publishes batches into the ring's data segment, the client decodes the rows."
echo "# Both engines run the same schema, the same seeded parameter stream and the same digest"
echo "# (count + column sums, the workload player's), so the rows are proven identical."
echo "# Only the doorbell differs: the shm engine posts a semaphore per handoff and the worker"
echo "# sleeps between them; the regipc engine's client makes an ioctl per request and both"
echo "# sides poll the ring's counters for up to 100 us before falling back to those same"
echo "# semaphores. The payload path - the data segment and its publish barriers - is shared."
echo "# Both sides' CPU is reported: the client's from its own clock, the worker's from"
echo "# /proc/<pid>/stat (utime+stime, 10 ms ticks) at the pid the ring names. The ring's tally"
echo "# says what the doorbell cost either side over the timed window - the handoffs, the"
echo "# semaphore posts, the falls back to the semaphore, and the microseconds spent polling -"
echo "# which is exactly what a change of wait policy would have to move."
echo "# What these numbers do NOT show: the network, and any concurrency: one client, one"
echo "# connection, one ring per process."
echo "### END TRANSPORT-E2E"
echo

failed=0
for engine in $engines; do
  for scenario in ${scenarios[@]+"${scenarios[@]}"}; do
    for rep in $(seq 1 "$reps"); do
      stem="$work/cell_${engine}_${scenario}_${rep}"
      extra=()
      if [ "$scenario" != scan ] && [ -n "$reads" ]; then extra=(--reads "$reads"); fi
      echo "# $engine / $scenario / rep $rep of $reps"
      rc=0
      docker exec -u postgres -e "FASTPOSTGRES_TRANSPORT=$engine" "$container" \
        python3 "$player" --scenario "$scenario" --scale "$scale" --seed "$seed" --rep "$rep" \
        ${extra[@]+"${extra[@]}"} >"$stem.out" 2>"$stem.err" || rc=$?
      printf '%s\n' "$rc" >"$stem.rc"
      # the player's lines that are the record: its header, one line per read,
      # and the evidence; the client's own decode diagnostics stay out of it
      grep -E '^(#|[0-9]+ |evidence )' "$stem.out" | sed 's/^/  /' || true
      if [ "$rc" != 0 ]; then
        # the players' stderr (the client's arena and decode counters) is shown
        # only when a cell fails; a passing cell's record is the lines above it
        if [ -s "$stem.err" ]; then sed 's/^/  ! /' "$stem.err"; fi
        echo "  FAILED $engine/$scenario rep $rep: the player exited $rc (see the ! lines)"
        failed=1
      fi
    done
  done
done

export BENCH_WORK="$work" BENCH_ENGINES="$engines" BENCH_SCENARIOS="${scenarios[*]}"
export BENCH_REPS="$reps" BENCH_SCALE="$scale" BENCH_SEED="$seed" BENCH_CONTAINER="$container"
export BENCH_CPU="$cpu" BENCH_MEM="$mem" BENCH_GHZ="$ghz" BENCH_CORES="$cores" BENCH_KERNEL="$kernel"
export BENCH_POSTGRES="$pg_version" BENCH_PYTHON="$py_version" BENCH_PSYCOPG="$psycopg_version"
export BENCH_IMAGE="$image $image_id" BENCH_SHM="$shm" BENCH_SEGMENT_MIB="$segment_mib"
export BENCH_POOL="$pool" BENCH_GIT="$gitrev" BENCH_REGIPC_MODE="$regipc_mode"
export BENCH_REGIPC_VERSION="$regipc_version" BENCH_REGIPC_SRCVERSION="$regipc_srcversion"
export BENCH_REGIPC_PARAMS="$regipc_params" BENCH_REGIPC_CONTAINER="$regipc_container"

# The tables and the one-line record. Everything below is aggregation and
# formatting over what the players already reported; nothing is timed twice.
python3 <<'PY'
import json
import os
import statistics
import subprocess
import sys

work = os.environ["BENCH_WORK"]
engines = os.environ["BENCH_ENGINES"].split()
scenarios = os.environ["BENCH_SCENARIOS"].split()
reps = int(os.environ["BENCH_REPS"])
container = os.environ["BENCH_CONTAINER"]
EXPECTED = {"shm": 1, "regipc": 2}

# The header from the container's /dev/shm, read by the harness itself: magic,
# version, transport id, data_bytes, client_pid, the regipc leaf's counter, and
# both sides' tally with the worker's pid (ring.h: FastpostgresRingTally at
# 4488). The player already read it; this is the second, independent look.
PROBE = """
import mmap, struct, sys
path = sys.argv[1]
try:
    with open(path, "rb") as fh, mmap.mmap(fh.fileno(), 0, prot=mmap.PROT_READ) as view:
        magic, version, ready, pid, transport, data_bytes = struct.unpack_from("<8sIIIIQ", view, 0)
        leaf_magic, leaf_calls = struct.unpack_from("<II", view, 4096)
        tally = struct.unpack_from("<8QII", view, 4488)
except OSError as exc:
    print("magic unreadable version - transport - data_bytes - client_pid - worker_ready - leaf_calls -", exc)
    raise SystemExit(0)
print("magic", magic.decode("latin-1"), "version", version, "transport", transport,
      "data_bytes", data_bytes, "client_pid", pid, "worker_ready", ready,
      "leaf_calls", leaf_calls if leaf_magic == 0x4C454146 else "-",
      "worker_pid", tally[8],
      "tally_handoffs", f"{tally[0]}/{tally[4]}", "tally_posts", f"{tally[1]}/{tally[5]}",
      "tally_sleeps", f"{tally[2]}/{tally[6]}", "tally_spin_us", f"{tally[3]}/{tally[7]}")
"""


def human(value):
    for unit, scale in (("G", 1e9), ("M", 1e6), ("k", 1e3)):
        if value >= scale:
            return f"{value / scale:.2f}{unit}".rstrip("0").rstrip(".")
    return f"{value:.0f}"


def same_digest(left, right, tolerance=1e-6):
    """The workload rule (lib/harness.py): integers exactly, floats near."""
    for a, b in zip(left, right):
        if isinstance(a, float) or isinstance(b, float):
            scale = abs(a) + abs(b)
            if scale and abs(a - b) > tolerance * scale:
                return False
        elif a != b:
            return False
    return True


def harness_ring(path):
    """The ring header as the harness reads it, plus whether it proves the engine."""
    if not path:
        return False, "the player named no ring"
    try:
        done = subprocess.run(["docker", "exec", "-i", container, "python3", "-", path],
                              input=PROBE, capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return False, f"could not read {path}: {exc}"
    text = (done.stdout or done.stderr).strip().replace("\n", " ")
    if done.returncode != 0:
        return False, f"{path}: {text}"
    fields = dict(zip(text.split()[0::2], text.split()[1::2]))
    ok = fields.get("magic") == "FPRING05" and fields.get("version") == "5"
    return ok, f"{path}: {text}"


def read_cell(engine, scenario, rep):
    stem = os.path.join(work, f"cell_{engine}_{scenario}_{rep}")
    samples, evidence, rc, err = [], None, None, ""
    try:
        with open(stem + ".out", encoding="utf-8") as fh:
            for line in fh:
                line = line.rstrip("\n")
                if line.startswith("evidence "):
                    evidence = json.loads(line[len("evidence "):])
                elif line and line[0].isdigit():
                    parts = line.split()
                    if len(parts) == 4:
                        samples.append((int(parts[0]), float(parts[1]), int(parts[2]), int(parts[3])))
    except OSError:
        pass
    try:
        with open(stem + ".rc", encoding="utf-8") as fh:
            rc = int(fh.read().strip())
    except (OSError, ValueError):
        pass
    try:
        with open(stem + ".err", encoding="utf-8") as fh:
            err = fh.read().strip()
    except OSError:
        pass
    return {"samples": samples, "evidence": evidence, "rc": rc, "err": err}


def throughput(ev):
    """The scenario's headline rate: delivered rows/s on the bulk scan, statements/s otherwise."""
    if ev["kind"] == "bulk":
        return ev["rows"] / (ev["p50_ms"] / 1000.0)
    return ev["reads"] / (ev["work_ms"] / 1000.0)


def median(values):
    return statistics.median(values) if values else 0.0


records, failures, mismatches = [], [], []
for scenario in scenarios:
    cells = {}
    for engine in engines:
        runs = []
        for rep in range(1, reps + 1):
            cell = read_cell(engine, scenario, rep)
            ev = cell["evidence"]
            problems = []
            if cell["rc"] != 0:
                tail = cell["err"].splitlines()[-1] if cell["err"] else ""
                problems.append(f"the player exited {cell['rc']}: {tail}")
            if ev is None:
                problems.append("no evidence line")
            else:
                if not ev["valid"]:
                    problems.extend(ev["problems"] or ["the player marked the cell invalid"])
                if len(cell["samples"]) != ev["reads"]:
                    problems.append(f"{len(cell['samples'])} per-read lines for {ev['reads']} reads")
                if any(s[3] != 1 for s in cell["samples"]):
                    problems.append("a per-read line says the ring did not serve it")
                if ev["served_by_shm"] != ev["reads"]:
                    problems.append(f"shm_reads advanced {ev['served_by_shm']} times "
                                    f"for {ev['reads']} reads")
                if ev["ring_transport"] != EXPECTED[engine]:
                    problems.append(f"the ring says transport {ev['ring_transport']}, "
                                    f"{engine} is {EXPECTED[engine]}")
                ok, text = harness_ring((ev.get("ring") or {}).get("path"))
                ev["ring_harness"] = text
                if not ok:
                    problems.append("the harness could not read the ring's header: " + text)
            runs.append({"rep": rep, "ev": ev, "problems": problems})
        cells[engine] = runs

    first = next((r["ev"] for engine in engines for r in cells[engine] if r["ev"]), None)
    if first is None:
        failures.append(f"{scenario}: no cell produced evidence")
        continue

    print()
    print(f"--- {scenario} ({first['kind']}): {first['note']} ---")
    print(f"# sql: {first['sql']}")
    unit = "rows/s" if first["kind"] == "bulk" else "stmts/s"
    print(f"# {first['reads']} timed read{'s' if first['reads'] != 1 else ''} per rep, {reps} rep(s),"
          f" one process per rep; unit {unit}; batch ask {first['batch']} rows"
          f" (the client's _SHM_BATCH)")
    print("# the ring, read by the harness itself from the container's /dev/shm:")
    for engine in engines:
        for run in cells[engine]:
            print(f"#   {engine}/rep{run['rep']} {(run['ev'] or {}).get('ring_harness', 'no evidence')}")

    digests = []
    for engine in engines:
        runs = cells[engine]
        evidences = [r["ev"] for r in runs if r["ev"]]
        if not evidences:
            print(f"  {engine:7s} FAILED (no evidence; see the ! lines above)")
            failures.append(f"{scenario}/{engine}")
            continue
        values = [throughput(ev) for ev in evidences]
        digests.extend(ev["digest"] for ev in evidences)
        leaf = [ev["leaf_calls_timed"] for ev in evidences if ev.get("leaf_calls_timed") is not None]
        row = {
            "scenario": scenario,
            "engine": engine,
            "unit": unit,
            "value": round(median(values), 3),
            "value_min": round(min(values), 3),
            "value_max": round(max(values), 3),
            "p50_ms": round(median([ev["p50_ms"] for ev in evidences]), 3),
            "p95_ms": round(median([ev["p95_ms"] for ev in evidences]), 3),
            "p99_ms": round(median([ev["p99_ms"] for ev in evidences]), 3),
            "reads": evidences[0]["reads"],
            "rows": int(median([ev["rows"] for ev in evidences])),
            "batch": evidences[0]["batch"],
            "batch_delivered": round(median([ev["batch_delivered"] for ev in evidences]), 1),
            "requests": int(median([ev["ring_requests"] for ev in evidences])),
            "ring_transport": evidences[0]["ring_transport"],
            "ring_data_bytes": evidences[0]["ring_data_bytes"],
            "served_by_shm": evidences[0]["served_by_shm"],
            "leaf_calls": int(median(leaf)) if leaf else None,
            "cpu_s": round(median([ev["cpu_s"] for ev in evidences]), 6),
            "cpu_us_per_row": round(median([ev["cpu_us_per_row"] for ev in evidences]), 4),
            # the worker: its CPU from /proc, and the tally both sides kept
            "worker_cpu_s": round(median([ev["worker_cpu_s"] for ev in evidences]), 6),
            "worker_cpu_us_per_row": round(median([ev["worker_cpu_us_per_row"] for ev in evidences]), 4),
            "client_handoffs": int(median([ev["client_handoffs"] for ev in evidences])),
            "client_posts": int(median([ev["client_posts"] for ev in evidences])),
            "client_sleeps": int(median([ev["client_sleeps"] for ev in evidences])),
            "client_spin_us": int(median([ev["client_spin_us"] for ev in evidences])),
            "worker_handoffs": int(median([ev["worker_handoffs"] for ev in evidences])),
            "worker_posts": int(median([ev["worker_posts"] for ev in evidences])),
            "worker_sleeps": int(median([ev["worker_sleeps"] for ev in evidences])),
            "worker_spin_us": int(median([ev["worker_spin_us"] for ev in evidences])),
            "rss_bytes": max(ev["rss_bytes"] for ev in evidences),
            "mapped_bytes": max(ev["mapped_bytes"] for ev in evidences),
            "work_ms": round(median([ev["work_ms"] for ev in evidences]), 3),
            "digest": evidences[0]["digest"],
            "valid": all(not r["problems"] for r in runs),
            "values": [round(v, 3) for v in values],
        }
        problems = [f"rep {r['rep']}: {p}" for r in runs for p in r["problems"]]
        if problems:
            row["valid"] = False
            failures.append(f"{scenario}/{engine}")
            for problem in problems:
                print(f"  {engine:7s} INVALID: {problem}")
        records.append(row)
        leaf_note = "--" if row["leaf_calls"] is None else f"{row['leaf_calls']}"
        print(f"  {engine:7s} {human(row['value']):>9s} {unit:7s}"
              f"   {human(row['value_min'])}..{human(row['value_max'])}"
              f"   p50 {row['p50_ms']:>9.3f}  p95 {row['p95_ms']:>9.3f}  p99 {row['p99_ms']:>9.3f} ms"
              f"   reads {row['reads']:>4d}  rows {row['rows']:>9,d}"
              f"  digest {'ok' if row['valid'] else 'INVALID'}")
        print(f"  {'':7s} {'batch':>12s} {row['batch']:>7d} rows asked, {row['batch_delivered']:.0f}"
              f" delivered, {row['requests']} ring requests; served_by_shm {row['served_by_shm']}/"
              f"{row['reads']}; ring transport {row['ring_transport']}, data_bytes"
              f" {row['ring_data_bytes']}; leaf_calls {leaf_note}; rss {row['rss_bytes'] / (1 << 20):.1f}"
              f" MiB; digest {row['digest']}")
        # The wait policy, per side: what each handoff cost the two processes.
        print(f"  {'':7s} {'wait':>12s} handoffs {row['client_handoffs']} posted / {row['worker_handoffs']}"
              f" answered; semaphore posts {row['client_posts']} client / {row['worker_posts']} worker;"
              f" sleeps {row['client_sleeps']} / {row['worker_sleeps']};"
              f" spin {row['client_spin_us']} / {row['worker_spin_us']} us")
        print(f"  {'':7s} {'cpu':>12s} {row['cpu_us_per_row']:.3f} us/row client ({row['cpu_s']:.6f} s),"
              f" {row['worker_cpu_us_per_row']:.3f} us/row worker ({row['worker_cpu_s']:.6f} s);"
              f" /proc utime+stime, 10 ms ticks, per run")

    by_engine = {row["engine"]: row for row in records if row["scenario"] == scenario}
    if len(by_engine) == 2 and all(row["value"] for row in by_engine.values()):
        base = by_engine["shm"]["value"]
        other = by_engine["regipc"]["value"]
        print(f"  {'regipc/shm':9s} {other / base:>10.3f} x  {(other / base - 1) * 100:+.1f}%"
              f"   (delivered {unit}: {human(base)} shm, {human(other)} regipc)")
        if by_engine["shm"]["p50_ms"]:
            print(f"  {'p50':9s} {by_engine['regipc']['p50_ms'] / by_engine['shm']['p50_ms']:>10.3f} x"
                  f"   per read: {by_engine['shm']['p50_ms']:.3f} ms shm,"
                  f" {by_engine['regipc']['p50_ms']:.3f} ms regipc")

    # The same rows, across every rep and both engines.
    same = len(digests) > 1 and all(same_digest(digests[0], other) for other in digests[1:])
    reference = digests[0] if digests else [0, 0, 0]
    if same:
        print(f"  {'digest':9s} the same in {len(digests)} cell(s): count={reference[0]:,}"
              f" sum={reference[1]:,} sum={reference[2]:,.3f}")
    else:
        mismatches.append(scenario)
        print(f"  {'digest':9s} MISMATCH: {digests}")

print()
print(f"# every timed read served by its ring (no libpq fallback): "
      f"{'ok' if not failures else 'FAILED'}")
print(f"# the same rows in every cell, both engines: "
      f"{'ok' if not mismatches else 'MISMATCH ' + ','.join(mismatches)}")
if len(engines) < 2:
    print(f"# WARNING the record has one engine ({' '.join(engines)}): this run does not"
          f" compare the two, by request")

machine = {
    "cpu": os.environ["BENCH_CPU"],
    "mem": os.environ["BENCH_MEM"],
    "ghz": os.environ["BENCH_GHZ"],
    "cores": int(os.environ["BENCH_CORES"]),
    "kernel": os.environ["BENCH_KERNEL"],
    "postgres": os.environ["BENCH_POSTGRES"],
    "python": os.environ["BENCH_PYTHON"] + " / psycopg " + os.environ["BENCH_PSYCOPG"],
    "image": os.environ["BENCH_IMAGE"],
    "shm": os.environ["BENCH_SHM"],
    "segment_mib": int(os.environ["BENCH_SEGMENT_MIB"]),
    "pool": int(os.environ["BENCH_POOL"]),
    "engines": engines,
    "scenarios": scenarios,
    "reps": reps,
    "scale": float(os.environ["BENCH_SCALE"]),
    "seed": int(os.environ["BENCH_SEED"]),
    "regipc": {
        "device": "/dev/regipc",
        "mode": os.environ["BENCH_REGIPC_MODE"],
        "module_version": os.environ["BENCH_REGIPC_VERSION"],
        "srcversion": os.environ["BENCH_REGIPC_SRCVERSION"],
        "params": os.environ["BENCH_REGIPC_PARAMS"],
        "in_container": os.environ["BENCH_REGIPC_CONTAINER"],
    },
    "git": os.environ["BENCH_GIT"],
}
print("### RESULTS")
print(json.dumps({"machine": machine, "scenarios": records}, separators=(",", ":")))
print("### END RESULTS")
if failures or mismatches:
    print("transport_e2e: %d cell(s) invalid, %d digest mismatch(es)"
          % (len(failures), len(mismatches)), file=sys.stderr)
    sys.exit(1)
PY

if [ "$failed" != 0 ]; then
  echo "transport_e2e: a player exited non-zero; see the FAILED lines above" >&2
  exit 1
fi
