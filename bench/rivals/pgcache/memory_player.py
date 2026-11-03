"""One equal-memory-budget measurement, in a process of its own.

Run inside the dev image, as postgres, by bench/harness/pgcache_memory_bench.sh:

    memory_player.py setup <n>
    memory_player.py read <player> <workload> <n> <reps> [--cold]

`setup` builds the same table (and the view the pass-through workload needs) the
plain PgCache comparison's player builds, and `read` runs the same two players,
workloads and digest: this file is that player with an evidence line, so the two
comparisons' numbers stay comparable.

The memory budget is the caller's to set, not an argument here: the ring runs
under FASTPOSTGRES_SEGMENT_BYTES (the capacity of each of the two alternating
data segments, so a stream's transfer buffer is twice it), and the cache under
the proxy's own MEMORY_LIMIT and MEMO_CACHE_SIZE. What this file adds is the
proof - the bytes the ring really mapped and the reads that really took it, and
the cache's own memory gauges - so the log checks the budgets instead of
asserting them.

Output, two lines:

    median_ms min_ms max_ms hits misses warm_reads warm_ms count id_sum price_sum stock_sum
    <evidence: key=value pairs for the player that just ran>
"""
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))  # bench/: lib
sys.path.insert(0, str(HERE))             # this directory: player

from lib.dataset import SHAPE, SHAPES  # noqa: E402
from lib.harness import rss_bytes  # noqa: E402
from player import PLAYERS, WORKLOADS, do_read, do_setup, scrape  # noqa: E402

# FASTPOSTGRES_RING_BYTES: the control channel's own segment, fixed and not part
# of the transfer buffer a budget is spent on.
CONTROL_RING_BYTES = 8 << 20


def ring_evidence(shm_reads, reps):
    """What the ring ran with, measured rather than asked for.

    The data segments are the worker's files under this process's stem; the
    harness clears /dev/shm before the cell, so a bigger segment left by an
    earlier budget cannot be read as this one's. `shm_reads` is the timed reads
    the client served from the ring: a read that fell back to libpq is missing
    from it, and a caller that finds it short of `reps` should not trust the
    timing.
    """
    stem = f"fp_dbapi_{os.getpid()}_"
    sizes = [
        os.stat(os.path.join("/dev/shm", name)).st_size
        for name in sorted(os.listdir("/dev/shm"))
        if name.startswith(stem) and "_seg_" in name
    ]
    return (
        f"seg_bytes={max(sizes) if sizes else 0} segments={len(sizes)} "
        f"pair_bytes={sum(sizes)} ctrl_bytes={CONTROL_RING_BYTES} "
        f"shm_reads={shm_reads}/{reps} rss_bytes={rss_bytes()}"
    )


def cache_evidence():
    """The proxy's own memory accounting: what budget it resolved (the asked
    MEMORY_LIMIT, or 80% of RAM when that is lower), what it is using, what the
    in-process memo holds, and whether memory pressure is throttling the
    registration of new queries."""
    gauges = scrape()

    def g(name):
        return int(gauges.get(name, 0.0))

    return (
        f"budget_bytes={g('pgcache_cache_memory_budget_bytes')} "
        f"used_bytes={g('pgcache_cache_memory_used_bytes')} "
        f"rss_bytes={g('pgcache_cache_rss_bytes')} "
        f"memo_bytes={g('pgcache_cache_memo_bytes')} "
        f"memo_hits={g('pgcache_cache_memo_hits')} "
        f"registered={g('pgcache_cache_queries_registered')} "
        f"throttled={g('pgcache_cache_registration_throttled')} "
        f"count_cap={g('pgcache_cache_query_count_cap')} "
        f"marginal_bytes={g('pgcache_cache_marginal_bytes_per_query')} "
        f"disk_used_bytes={g('pgcache_cache_disk_used_bytes')}"
    )


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    if argv[1] == "setup":
        do_setup(int(argv[2]))
        return
    if argv[1] == "read":
        player, workload = argv[2], argv[3]
        n, reps = int(argv[4]), int(argv[5])
        if player not in PLAYERS:
            raise SystemExit(f"unknown player {player!r}: {sorted(PLAYERS)}")
        if workload not in WORKLOADS:
            raise SystemExit(f"unknown workload {workload!r}: {sorted(WORKLOADS)}")
        if SHAPE not in SHAPES:
            raise SystemExit(f"SHAPE={SHAPE!r}: choose one of {sorted(SHAPES)}")
        evidence = {}
        print(" ".join(do_read(player, workload, n, reps, "--cold" in argv, evidence)))
        if player == "fastpostgres":
            print("ring " + ring_evidence(evidence["shm_reads"], reps))
        else:
            print("cache " + cache_evidence())
        return
    raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
