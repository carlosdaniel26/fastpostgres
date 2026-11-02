"""Run the workload suite: schema, scenarios, both players, one log.

Every scenario runs each player in a process of its own (several, for the
concurrency and mixed scenarios), compares the digests across players, and prints
one block: the measurement per player - throughput, or ms for a single expensive
read, with p50/p95/p99 - the evidence that the work took the path it claims, and
the digest check.

    runner.py [scenario...] [--quick] [--scale=1] [--reps=3] [--duration=10]
              [--clients=4] [--seed=7]

The rival is psycopg 3, the plain driver the same application ran on before, and
both sides use the same app-facing call: `execute(...).fetchall()`. This answers
"does swapping the driver help my application", not "which transport is faster";
the other stacks are the rivals class (`just rivals`).

Scale 1 is 10k customers, 10k products, 1M orders and 3M order items; `--quick`
is a tenth of that with short windows, for a smoke run.
"""
import argparse
import importlib.metadata
import json
import os
import subprocess
import sys
from pathlib import Path

import psycopg

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # benchmarks/

from lib.harness import percentile, same_digest  # noqa: E402
from scenarios import BY_NAME, SCENARIOS  # noqa: E402
from schema import DATABASE_URL, reset_reserved_orders, setup  # noqa: E402

HERE = Path(__file__).resolve().parent
PLAYER = HERE / "player.py"
PLAYERS = ("fastpostgres", "psycopg 3")
DEFAULTS = {"scale": 1.0, "reps": 3, "duration": 10.0, "clients": 4, "seed": 7}


def server_version():
    conn = psycopg.connect(DATABASE_URL)
    try:
        with conn.cursor() as cur:
            cur.execute("SHOW server_version")
            return cur.fetchone()[0].split()[0]
    finally:
        conn.close()


def env_block(n, args):
    def proc(path):
        with open(path) as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
                if line.startswith("MemTotal"):
                    return f"{int(line.split()[1]) / 1048576:.0f} GiB"
        return "?"

    print("### ENV")
    print(f"cpu: {proc('/proc/cpuinfo')}")
    print(f"mem: {proc('/proc/meminfo')}")
    print(f"cores: {os.cpu_count()}")
    print(f"postgres: {server_version()}")
    print(f"python: {sys.version.split()[0]}")
    print(f"psycopg: {importlib.metadata.version('psycopg')}")
    print(f"fastpostgres image: {os.environ.get('BENCH_IMAGE', '?')}")
    print(f"shm: {os.environ.get('BENCH_SHM', '?')}")
    print(f"ring segment: {os.environ.get('BENCH_SEGMENT', 'default')}")
    print(f"scale: {args.scale:g} ({n['customers']} customers, {n['products']} products, "
          f"{n['orders']} orders, {n['orders'] * n['items_per_order']} order items)")
    print(f"seed: {args.seed}")
    print(f"git: {os.environ.get('BENCH_GIT_REV', 'unknown')}")
    print("### END ENV")


def spawn_players(player, scenario, args, clients=1):
    """Run one player's scenario: one process, or `clients` of them at once."""
    argv = [
        sys.executable, str(PLAYER),
        "--scenario", scenario.name,
        "--player", player,
        "--scale", str(args.scale),
        "--reps", str(args.reps),
        "--duration", str(args.duration),
        "--clients", str(clients),
        "--seed", str(args.seed),
    ]
    if clients == 1:
        proc = subprocess.run(argv, capture_output=True, text=True)
        if proc.returncode != 0:
            raise SystemExit(f"{player}/{scenario.name} failed:\n{proc.stderr[-3000:]}")
        return [json.loads(proc.stdout.strip().splitlines()[-1])]

    running = []
    for index in range(clients):
        argv_i = [*argv, "--client-index", str(index)]
        running.append(subprocess.Popen(argv_i, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True))
    results = []
    for proc in running:
        out, err = proc.communicate()
        if proc.returncode != 0:
            raise SystemExit(f"{player}/{scenario.name} client failed:\n{err[-3000:]}")
        results.append(json.loads(out.strip().splitlines()[-1]))
    return results


def merge(clients):
    """Several client results as one: summed counts and evidence, merged timings."""
    first = clients[0]
    samples = [ms for c in clients for ms in c["samples"]]
    write_samples = [ms for c in clients for ms in c["write_samples"]]
    return {
        "player": first["player"],
        "reads": sum(c["reads"] for c in clients),
        "writes": sum(c["writes"] for c in clients),
        "rows_per_read": first["rows_per_read"],
        "work_ms": max(c["work_ms"] for c in clients),
        "digest": [sum(c["digest"][i] for c in clients) for i in range(3)],
        "valid": all(c["valid"] for c in clients),
        "shm_reads": sum(c["shm_reads"] for c in clients),
        "pool": [c["pool"] for c in clients],
        "mapped_bytes": sum(c["mapped_bytes"] for c in clients),
        "rss_bytes": max(c["rss_bytes"] for c in clients),
        "reads_ms": (percentile(samples, 0.5), percentile(samples, 0.95), percentile(samples, 0.99)),
        "writes_ms": (percentile(write_samples, 0.5), percentile(write_samples, 0.95),
                      percentile(write_samples, 0.99)),
    }


def headline(scenario, result):
    """The scenario's numbers, as (unit, value) pairs: throughput, or ms for a read."""
    seconds = result["work_ms"] / 1000
    values = []
    if scenario.kind == "duration" and scenario.write_sql:
        values.append(("reads/s", result["reads"] / seconds))
        values.append(("writes/s", result["writes"] / seconds))
    elif scenario.kind == "statements" or scenario.kind == "duration":
        values.append(("stmt/s", result["reads"] / seconds))
    elif scenario.name == "aggregate":
        values.append(("ms/read", result["reads_ms"][0]))
    else:
        # one read's rows over one read's median: the digest counts every rep
        rows = result["rows_per_read"] or result["digest"][0]
        values.append(("rows/s", rows / (result["reads_ms"][0] / 1000)))
    return values


def print_result(scenario, result, baseline):
    for index, (unit, value) in enumerate(headline(scenario, result)):
        p50, p95, p99 = result["writes_ms"] if index else result["reads_ms"]
        delta = ""
        if index == 0 and baseline is not None and baseline:
            delta = f"{(value / baseline - 1) * 100:+.0f}%"
        print(f"  {scenario.name:12s} {result['player']:12s} {value:>9.1f} {unit:8s}"
              f"  p50 {p50:>7.2f} ms  p95 {p95:>7.2f} ms  p99 {p99:>7.2f} ms  {delta:>7s}")

    pool = result["pool"]
    pool_note = pool if isinstance(pool, int) else ",".join(str(p) for p in pool)
    path = "ring " if result["player"] == "fastpostgres" else "session"
    print(f"  {'':12s} {result['player']:12s} {path}  "
          f"reads={result['reads']} writes={result['writes']} "
          f"served_by_shm={result['shm_reads']} pool={pool_note} "
          f"mapped_bytes={result['mapped_bytes']} rss_bytes={result['rss_bytes']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenarios", nargs="*", help="scenario names (default: all)")
    ap.add_argument("--quick", action="store_true", help="a tenth of the scale, short windows")
    ap.add_argument("--scale", type=float, default=None)
    ap.add_argument("--reps", type=int, default=None)
    ap.add_argument("--duration", type=float, default=None)
    ap.add_argument("--clients", type=int, default=None)
    ap.add_argument("--seed", type=int, default=DEFAULTS["seed"])
    args = ap.parse_args()

    unknown = [name for name in args.scenarios if name not in BY_NAME]
    if unknown:
        raise SystemExit(f"unknown scenario(s) {unknown}: {sorted(BY_NAME)}")
    if args.quick:
        args.scale = args.scale or 0.1
        args.reps = args.reps or 1
        args.duration = args.duration or 2.0
    args.scale = args.scale if args.scale is not None else DEFAULTS["scale"]
    args.reps = args.reps if args.reps is not None else DEFAULTS["reps"]
    args.duration = args.duration if args.duration is not None else DEFAULTS["duration"]
    args.clients = args.clients or DEFAULTS["clients"]

    selected = [BY_NAME[name] for name in args.scenarios] if args.scenarios else list(SCENARIOS)
    n = setup(args.scale)
    env_block(n, args)

    mismatches = 0
    records = []
    for scenario in selected:
        print()
        if scenario.kind == "duration":
            clients = args.clients if scenario.name == "concurrent" else 1
            detail = f"{clients} client(s) x {args.duration:g}s"
        elif scenario.kind == "bulk":
            detail = f"{args.reps} rep(s) of one statement"
        else:
            detail = f"{scenario.statements} statements x {args.reps} reps"
        print(f"# scenario {scenario.name} ({scenario.klass}): {scenario.note}, {detail}")
        if scenario.kind == "duration":
            print("# (each client runs its own window from a seeded, disjoint parameter stream)")
        print(f"  {'':12s} {'':12s} {'':9s} {'':8s}  {'p50':>10s}  {'p95':>10s}  {'p99':>10s}"
              f"  {'vs psycopg 3':>12s}")

        results = {}
        for player in PLAYERS:
            if scenario.name == "mixed":
                reset_reserved_orders()  # both players write the same reserved id range
            clients = args.clients if scenario.name == "concurrent" else 1
            results[player] = merge(spawn_players(player, scenario, args, clients))

        baseline = None
        for player in PLAYERS:
            print_result(scenario, results[player], baseline)
            if baseline is None:
                baseline = headline(scenario, results[player])[0][1]
            for index, (unit, value) in enumerate(headline(scenario, results[player])):
                times = results[player]["writes_ms"] if index else results[player]["reads_ms"]
                p50, p95, p99 = times
                records.append({
                    "scenario": scenario.name, "class": scenario.klass,
                    "player": player, "unit": unit, "value": round(value, 3),
                    "p50_ms": round(p50, 3), "p95_ms": round(p95, 3), "p99_ms": round(p99, 3),
                    "reads": results[player]["reads"], "writes": results[player]["writes"],
                    "served_by_shm": results[player]["shm_reads"],
                    "digest": results[player]["digest"], "valid": results[player]["valid"],
                })

        ring = results["fastpostgres"]
        if ring["shm_reads"] < ring["reads"]:
            print(f"  WARNING {ring['reads'] - ring['shm_reads']} of {ring['reads']} reads "
                  f"fell back to libpq (ring pool or worker budget): their timing is not the ring's")

        digests = [results[p]["digest"] for p in PLAYERS]
        valid = all(results[p]["valid"] for p in PLAYERS)
        if scenario.kind == "duration":
            # the windows stop on a clock, so each player issues a different number
            # of statements: there is no total to compare, only each statement's check
            if valid:
                print(f"  {scenario.name:12s} digest validated per statement "
                      f"(a window's statement count is not comparable)")
            else:
                print(f"  {scenario.name:12s} INVALID: a statement returned the wrong rows")
                mismatches += 1
        elif same_digest(*digests) and valid:
            print(f"  {scenario.name:12s} digest ok "
                  f"(count={digests[0][0]:,}, sum={digests[0][1]:,}, sum={digests[0][2]:,.1f})")
        else:
            print(f"  {scenario.name:12s} MISMATCH (valid={valid}, "
                  f"{PLAYERS[0]}={digests[0]}, {PLAYERS[1]}={digests[1]})")
            mismatches += 1

    print()
    print(f"# the same output across players: {'ok' if mismatches == 0 else 'MISMATCH'}")
    # the machine-readable record of the same run: the log is for reading, this
    # block is for the chart and for anything that wants the numbers without
    # parsing the prose
    print("### RESULTS")
    print(json.dumps({"git": os.environ.get("BENCH_GIT_REV", "unknown"),
                      "scale": args.scale, "scenarios": records}))
    print("### END RESULTS")
    return 1 if mismatches else 0


if __name__ == "__main__":
    raise SystemExit(main())
