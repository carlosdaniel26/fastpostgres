# Rivals

The competitive comparisons: what the ring does against the other stacks that
answer the same question, with each rival's driver committed next to the
comparison that drives it, so a clone shows both sides.

| rival | the question | the driver | the record |
|---|---|---|---|
| **psycopg 3** | does swapping the driver help an application? The workload suite runs the same commerce schema and the same app-facing call on both, so the comparison is the driver and nothing else | [`../workloads/`](../workloads/) (`--player psycopg 3`) | [`fastpostgres-vs-psycopg.log`](../results/fastpostgres-vs-psycopg.log) |
| **ADBC + Arrow** | the same read from the Arrow ecosystem's own answer: tuples against tuples, columnar against columnar | [`adbc-arrow/`](adbc-arrow/) | [`fastpostgres-vs-arrow.log`](../results/fastpostgres-vs-arrow.log), `-mixed`, `-small` |
| **ADBC + Arrow, under a budget** | the same reads with the ring's transfer buffer fixed: what each side holds while it delivers | [`adbc-arrow/arrow_memory_bench.py`](adbc-arrow/arrow_memory_bench.py) | [`fastpostgres-vs-arrow-memory.log`](../results/fastpostgres-vs-arrow-memory.log) |
| **PgCache** | a result cache removes execution; the ring removes delivery. Each wins a different shape | [`pgcache/`](pgcache/) | [`fastpostgres-vs-pgcache.log`](../results/fastpostgres-vs-pgcache.log) |
| **PgCache, one budget** | the same, with the same bytes spent on each side | [`pgcache/memory_player.py`](pgcache/memory_player.py) | [`fastpostgres-vs-pgcache-memory.log`](../results/fastpostgres-vs-pgcache-memory.log) |

Every one of them obeys the class rules in [`../README.md`](../README.md): one
player per process, one at a time; the same output on both sides, checked and
failing the run when it disagrees; an evidence line per cell saying how the work
actually travelled; and a committed log with the commit it came from. What each
comparison's rival is, how it is driven and which knob would most flatter it is
in that rival's own README, not here.

`just rivals` lists the targets; [`../METHODOLOGY.md`](../METHODOLOGY.md) is the
protocol and its threats to validity.
