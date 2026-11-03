"""One process, one engine, one scenario: a read delivered end to end.

Run by bench/harness/transport_e2e_bench.sh, never by hand:

    FASTPOSTGRES_TRANSPORT=regipc e2e_player.py --scenario scan --rep 1

It is one cell of the end-to-end record, and it is one process because it has to
be: FASTPOSTGRES_TRANSPORT is read at import (dbapi) and a ring's engine is
fixed when the client creates it, so a process that ran one engine cannot be
reused for the other. The harness starts one of these per (engine, scenario,
rep) and passes the knob in the environment.

What it does, in the order the workload player does it (bench/workloads/
player.py): ensure the scenario's rows exist, connect, warm up (connections, the
ring, plans - one read for a bulk scan, twenty for the statement scenarios), then
time the reads and fold their digest. It prints:

  * `# ...` header lines: the scenario, its SQL, the engine and the ring
    transport id it must appear as, the sizes, the seed and the rep - so the log
    says what was run;
  * `<i> <ms> <rows> <served_by_shm>`, one line per timed read, so the
    percentiles in the evidence line can be recomputed from the log;
  * `evidence {json}`, one line: the timings, the client process's CPU, peak
    RSS, the digest, and the ring's own header read from /dev/shm - `transport`
    (1 shm, 2 regipc), `data_bytes`, the requests posted, and, for regipc, the
    module's `leaf.calls` counter from the page it wrote in ring 0.

The transport is proven twice, and either check failing makes the cell invalid
(this process then exits non-zero, and the harness fails the run):

  * `conn.shm_reads` must advance by exactly one per timed read. A read the ring
    did not serve falls back to libpq and does not count;
  * the ring's header must name the engine this process asked for, and the ring
    must be the process's own (`client_pid`), so a segment made by another
    engine or another process cannot stand in.

The ring layout read here is extension/include/fastpostgres/ring.h, the ABI the
client and the worker share: magic[8], version, worker_ready, client_pid,
transport, data_bytes, req_seq, resp_seq, ...; `batch` at 128; the regipc leaf
on its own page at 4096, its `calls` at +4 (regipc.h: struct
regipc_leaf_result { magic; calls; ... }).
"""
import argparse
import json
import mmap
import os
import random
import struct
import sys
import time
from pathlib import Path

import psycopg

from fastpostgres import dbapi

BENCH = Path(__file__).resolve().parents[2]  # bench/
for path in (BENCH / "workloads", BENCH):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

from lib.harness import mapped_segment_bytes, percentile, rss_bytes  # noqa: E402
from schema import DATABASE_URL, counts, setup  # noqa: E402
from scenarios import BY_NAME  # noqa: E402

# The scenarios, from the workload suite's own specs: the same SQL, the same
# seeded parameter streams and the same digest, so this record reads exactly the
# rows `just bench` reads and the two logs can be compared.
SCENARIOS = ("scan", "lookup", "paged")
# Timed reads per rep. The scan is one read of every row (a bulk statement, like
# the workload suite's bulk scenarios); the statement scenarios issue `reads`
# statements per rep from a stream seeded without the engine's name, so both
# engines run the same statements in the same order.
DEFAULT_READS = {"scan": 1, "lookup": 500, "paged": 500}

# The ring's own header, as the client and the worker lay it out. The batch field
# is `uint32_t batch` after magic/version/worker_ready/client_pid/transport/
# data_bytes/req_seq/resp_seq/sem_req/sem_resp/op/status/nrows.
RING_MAGIC = b"FPRING04"
RING_VERSION = 4
RING_HEADER = "<8sIIIIQQQ"  # 0..47: magic, version, worker_ready, client_pid, transport, data..., ...
RING_BATCH = 128
RING_LEAF = 4096
RING_LEAF_CALLS = RING_LEAF + 4
LEAF_MAGIC = 0x4C454146  # "LEAF", extension/include/fastpostgres/regipc.h

TRANSPORT_ID = {"shm": 1, "regipc": 2}


def now():
    return time.perf_counter()


def cpu_seconds():
    """CPU this process has used, in seconds: user + system, every thread.

    time.process_time() and not os.times(): the statement scenarios time a window
    of a few milliseconds, and times(2)'s 10 ms tick would round that CPU to zero.
    """
    return time.process_time()


def ring_files(pid=None):
    """The ring control segments this process created, in /dev/shm.

    The client names them `fp_dbapi_<pid>_<index>` and their data segments
    `..._seg_a`/`_seg_b`, so the control segments are the ones without `_seg`.
    """
    stem = f"fp_dbapi_{pid if pid is not None else os.getpid()}_"
    found = []
    for name in sorted(os.listdir("/dev/shm")):
        if name.startswith(stem) and "_seg" not in name:
            found.append(os.path.join("/dev/shm", name))
    return found


def read_ring(path):
    """The ring header at `path`, as a dict (and the regipc leaf when it is one)."""
    with open(path, "rb") as fh:
        with mmap.mmap(fh.fileno(), 0, prot=mmap.PROT_READ) as view:
            (magic, version, worker_ready, client_pid, transport, data_bytes,
             req_seq, resp_seq) = struct.unpack_from(RING_HEADER, view, 0)
            (batch,) = struct.unpack_from("<I", view, RING_BATCH)
            leaf_magic, leaf_calls = struct.unpack_from("<II", view, RING_LEAF)
    ring = {
        "path": path,
        "magic": magic.decode("latin-1"),
        "version": version,
        "worker_ready": worker_ready,
        "client_pid": client_pid,
        "transport": transport,
        "data_bytes": data_bytes,
        "req_seq": req_seq,
        "resp_seq": resp_seq,
        "batch": batch,
        "leaf_magic": leaf_magic,
        "leaf_calls": leaf_calls if leaf_magic == LEAF_MAGIC else None,
    }
    if ring["magic"] != RING_MAGIC.decode("latin-1"):
        raise SystemExit(f"ring {path}: magic {ring['magic']!r}, not {RING_MAGIC!r}")
    if ring["version"] != RING_VERSION:
        raise SystemExit(f"ring {path}: version {ring['version']}, not {RING_VERSION}")
    return ring


def find_ring():
    """The one ring this process created; an error if there is not exactly one.

    The harness pins FASTPOSTGRES_SHM_POOL=1, so one process has one ring and the
    evidence can name it. More than one would mean a read was served by a ring
    the evidence did not check, which is a failed cell rather than a footnote.
    """
    paths = ring_files()
    if len(paths) != 1:
        raise SystemExit(f"expected one ring for pid {os.getpid()} in /dev/shm, found {paths}")
    return read_ring(paths[0])


def ensure_schema(scale):
    """Build the scenario's rows unless the database already holds them.

    The rows are the commerce schema the workload suite reads (the same
    deterministic load), so both engines read the same data and this record's
    scan is comparable with `just bench`'s. A rebuild is only when the row count
    is not the one this scale asks for - a fresh database, or another scale -
    so the first cell of a run pays for it and the rest reuse it.
    """
    wanted = counts(scale)
    have = -1
    try:
        conn = psycopg.connect(DATABASE_URL)
        try:
            conn.autocommit = True
            with conn.cursor() as cur:
                cur.execute("SELECT count(*) FROM orders")
                have = int(cur.fetchone()[0])
        finally:
            conn.close()
    except Exception:  # no database, no tables: build them
        have = -1
    if have == wanted["orders"]:
        return wanted, False
    setup(scale)
    return wanted, True


def measure(conn, scenario, wanted, args):
    """The scenario's reads; returns the samples, the digest and the CPU they took."""
    cur = conn.cursor()
    rng = random.Random(f"e2e:{scenario.name}:{args.seed}")  # no engine, no rep: the same stream
    samples, served, lines = [], 0, []
    rows_total = 0
    ok, count, acc_a, acc_b = True, 0, 0, 0

    def read(index, timed=False):
        """One statement of the scenario; timed reads are the ones the log carries.

        The warm-up reads are not printed, and the per-read lines are printed
        after the timed window rather than as they are taken: writing to stdout
        between two reads would widen the gap the worker sleeps in (and, for the
        statement scenarios, land in the window the rate is taken over). The
        log's `read i` lines are therefore exactly the timed sample.
        """
        nonlocal rows_total, served
        param = scenario.param(index, wanted, rng)
        before = conn.shm_reads
        started = now()
        cur.execute(scenario.sql, param)
        rows = cur.fetchall()
        ms = (now() - started) * 1000
        on_ring = conn.shm_reads - before
        served += on_ring
        rows_total += len(rows)
        if timed:
            lines.append((index, ms, len(rows), on_ring))
        good, c, x, y = scenario.digest(rows, param)
        return ms, good, c, x, y

    try:
        warmup = 1 if scenario.kind == "bulk" else 20
        for i in range(warmup):
            read(i)  # warm: the connection, the ring, the worker, the plan
        served = 0  # the evidence counts the timed reads, not the warm-up
        shm_before = conn.shm_reads
        rows_total = 0
        ring_before = read_ring(find_ring()["path"])
        cpu_before = cpu_seconds()
        started = now()
        if scenario.kind == "bulk":
            param = scenario.param(0, wanted, rng)
            for i in range(args.reads):
                before = conn.shm_reads
                inner = now()
                cur.execute(scenario.sql, param)
                rows = cur.fetchall()
                ms = (now() - inner) * 1000
                on_ring = conn.shm_reads - before
                served += on_ring
                rows_total += len(rows)
                samples.append(ms)
                lines.append((i, ms, len(rows), on_ring))
                good, c, x, y = scenario.digest(rows, param)
                ok &= good
                count += c
                acc_a += x
                acc_b += y
        else:
            for i in range(args.reads):
                ms, good, c, x, y = read(i, timed=True)
                samples.append(ms)
                ok &= good
                count += c
                acc_a += x
                acc_b += y
        work_ms = (now() - started) * 1000
        cpu_s = cpu_seconds() - cpu_before
        reads = args.reads
        shm_reads = conn.shm_reads - shm_before
        for index, ms, rows, on_ring in lines:
            print(f"{index} {ms:.3f} {rows} {on_ring}")
        sys.stdout.flush()
        ring_after = read_ring(ring_before["path"])
    finally:
        cur.close()

    expected = TRANSPORT_ID[dbapi.transport()]
    ring_requests = ring_after["req_seq"] - ring_before["req_seq"]
    # Per read the client posts OPEN, one NEXT per payload batch plus the one
    # NEXT that returns 0 (it asks for the next batch before decoding the
    # current, so the end of the stream is found by asking again), and CLOSE.
    payload_batches = ring_requests - 3 * reads
    leaf_timed = None
    if ring_after["leaf_calls"] is not None and ring_before["leaf_calls"] is not None:
        leaf_timed = ring_after["leaf_calls"] - ring_before["leaf_calls"]
    problems = []
    if shm_reads != reads:
        problems.append(f"{reads - shm_reads} of {reads} timed reads fell back to libpq")
    if ring_after["transport"] != expected:
        problems.append(f"the ring says transport {ring_after['transport']}, "
                        f"{dbapi.transport()} is {expected}")
    if ring_after["client_pid"] != os.getpid():
        problems.append(f"the ring belongs to pid {ring_after['client_pid']}, not {os.getpid()}")
    if served != reads:
        problems.append(f"the per-read evidence counted {served} ring reads, not {reads}")
    if expected == TRANSPORT_ID["regipc"] and not (leaf_timed and leaf_timed >= reads):
        problems.append(f"the module's leaf counted {leaf_timed} timed calls for {reads} reads: "
                        f"ring 0 did not run")
    if not ok:
        problems.append("a read returned the wrong rows")

    return {
        "scenario": scenario.name,
        "kind": scenario.kind,
        "note": scenario.note,
        "sql": " ".join(scenario.sql.split()),
        "engine": dbapi.transport(),
        "rep": args.rep,
        "reads": reads,
        "rows": rows_total,
        "samples_ms": [round(ms, 3) for ms in samples],
        "p50_ms": round(percentile(samples, 0.50), 3),
        "p95_ms": round(percentile(samples, 0.95), 3),
        "p99_ms": round(percentile(samples, 0.99), 3),
        "work_ms": round(work_ms, 3),
        "cpu_s": round(cpu_s, 6),
        "cpu_us_per_row": round(cpu_s * 1e6 / rows_total, 6) if rows_total else 0.0,
        "served_by_shm": shm_reads,
        "pool": dbapi.pool_size(),
        "pool_created": dbapi.pool_created(),
        "mapped_bytes": mapped_segment_bytes(),
        "rss_bytes": rss_bytes(),
        "batch": dbapi._SHM_BATCH,
        "ring_requests": ring_requests,
        "payload_batches": payload_batches,
        "batch_delivered": round(rows_total / payload_batches, 1) if payload_batches > 0 else 0.0,
        "ring": ring_after,
        "ring_transport": ring_after["transport"],
        "ring_data_bytes": ring_after["data_bytes"],
        "leaf_calls": ring_after["leaf_calls"],
        "leaf_calls_timed": leaf_timed,
        "digest": [count, acc_a, round(acc_b, 3)],
        "valid": not problems,
        "problems": problems,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenario", required=True, choices=sorted(SCENARIOS))
    ap.add_argument("--reads", type=int, default=None, help="timed reads per rep")
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--rep", type=int, default=1, help="this rep's number, for the log only")
    args = ap.parse_args()

    scenario = BY_NAME[args.scenario]
    args.reads = args.reads if args.reads is not None else DEFAULT_READS[scenario.name]
    engine = dbapi.transport()
    asked = os.environ.get("FASTPOSTGRES_TRANSPORT")
    if asked is not None and asked != engine:
        raise SystemExit(f"FASTPOSTGRES_TRANSPORT={asked!r} but the client imported {engine!r}")

    wanted, rebuilt = ensure_schema(args.scale)
    print(f"# e2e player: scenario {scenario.name} ({scenario.kind}), {scenario.note}")
    print(f"# sql: {' '.join(scenario.sql.split())}")
    print(f"# engine: {engine} (FASTPOSTGRES_TRANSPORT={asked or 'unset'}); the ring must say "
          f"transport {TRANSPORT_ID[engine]}")
    print(f"# scale: {args.scale:g} ({wanted['customers']} customers, {wanted['orders']} orders), "
          f"schema {'rebuilt' if rebuilt else 'reused'}, seed {args.seed}, rep {args.rep}")
    print(f"# reads: {args.reads} timed per rep, {1 if scenario.kind == 'bulk' else 20} warm-up")
    print("# read i: <index> <ms> <rows> <served_by_shm>")

    conn = dbapi.connect(DATABASE_URL)
    try:
        evidence = measure(conn, scenario, wanted, args)
    finally:
        conn.close()
    print("evidence " + json.dumps(evidence, separators=(",", ":")))
    if not evidence["valid"]:
        for problem in evidence["problems"]:
            print(f"e2e_player: INVALID: {problem}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
