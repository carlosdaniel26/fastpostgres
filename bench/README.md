# Bench: what is measured, in which class, and where the record lives

The claim's tree. It holds the apparatus (the harnesses, the dispatchers, the
chart scripts, the image jobs), the arguments the measurements are for
([`WHY-IT-WINS.md`](WHY-IT-WINS.md), [`PG-CACHE.md`](PG-CACHE.md),
[`ARROW-MEMORY.md`](ARROW-MEMORY.md), [`CODEC-TRANSPORT.md`](CODEC-TRANSPORT.md)),
the method and its threats ([`METHODOLOGY.md`](METHODOLOGY.md)), the lab
notebook ([`NOTES.md`](NOTES.md)), and the records themselves.

Every number the project quotes is a line in a committed log under
[`results/`](results/) with a `git:` stamp of the commit it was produced from;
[`RESULTS.md`](RESULTS.md) indexes every record and the claim it backs.

| class | command | the question it answers |
|---|---|---|
| rivals | `just rivals [target]` | how the ring compares with the other stacks (ADBC + Arrow, a result cache), at their defaults and under a fixed memory budget |
| internals | `just internals [target]` | what the row decode and the transport cost at the instruction, cache and port level - the evidence the design notes argue from |
| adoption | `just adoption [target]` | what a published suite's own queries and protocol find, with our cell in its roster - [`adoption/`](adoption/), written up in [`ADOPTION.md`](ADOPTION.md) |

Rules every class follows, because a measurement that breaks them is not one:

- **One player per process, one at a time.** Interleaving players measures the
  neighbours' cache footprint (see [`METHODOLOGY.md`](METHODOLOGY.md)).
- **The same output, checked.** Every comparison defines what "the same" means -
  the same read and the same row count, the same tuples, a checksum over the
  decoded values - and fails the run when it disagrees.
- **Evidence per cell.** A player says how its work actually travelled: reads
  served by the ring against reads issued, the ring pool, the bytes it mapped,
  its peak RSS, the transfer segment it was given. A fallback to libpq that the
  timing would otherwise hide is printed.
- **A committed log, with its stamp.** `git describe --always --dirty` and the
  shape/scale, so a published number points back at the code and the data.

Nothing is deleted from the history: the old recipe names still exist as targets
of the dispatchers below, and their logs remain the record.

## 1. Rivals: `just rivals [target]`

```
just rivals                  # list the targets
just rivals adbc             # the ADBC + Arrow comparison, unchanged
just rivals cache --quick
```

| target | the question | record | method and caveats |
|---|---|---|---|
| `adbc` | the read against the Arrow ecosystem's own answer, same output | [`fastpostgres-vs-arrow.log`](results/fastpostgres-vs-arrow.log), [`-mixed`](results/fastpostgres-vs-arrow-mixed.log), [`-small`](results/fastpostgres-vs-arrow-small.log) | [`METHODOLOGY.md`](METHODOLOGY.md), [`WHY-IT-WINS.md`](WHY-IT-WINS.md), [`LIMITATIONS.md`](../docs/LIMITATIONS.md) |
| `arrow-memory` | the same reads with the ring's buffer fixed: what each side holds | [`fastpostgres-vs-arrow-memory.log`](results/fastpostgres-vs-arrow-memory.log) | [`ARROW-MEMORY.md`](ARROW-MEMORY.md) |
| `cache` | a CDC-maintained result cache in front of the same server, its defaults | [`fastpostgres-vs-pgcache.log`](results/fastpostgres-vs-pgcache.log) | [`PG-CACHE.md`](PG-CACHE.md) |
| `cache-budget` | the same, with one memory budget on both sides | [`fastpostgres-vs-pgcache-memory.log`](results/fastpostgres-vs-pgcache-memory.log) | [`PG-CACHE.md#the-same-memory-budget-on-both-sides`](PG-CACHE.md#the-same-memory-budget-on-both-sides) |

## 2. Internals: `just internals [target]`

These are not benchmarks of the product. They are the meters behind
[`WHY-IT-WINS.md`](WHY-IT-WINS.md) and
[`CODEC-TRANSPORT.md`](CODEC-TRANSPORT.md) - how many instructions a row costs,
which ports the decode loop uses, what a format costs versus what a transport
costs - and their results live in the lab notebook rather than in this catalog.

| target | the meter | results in |
|---|---|---|
| `decode-cycles` | TSC cycles per row on a fixed shape | [`NOTES.md`](NOTES.md) |
| `decode-ir` | dynamic instructions, branch and cache misses, IPC (callgrind; profile image) | [`NOTES.md`](NOTES.md) |
| `decode-mca` | the decode loop's ports and latency, modeled offline (llvm-mca; profile image) | [`NOTES.md`](NOTES.md) |
| `codec-transport` | format cost and transport cost, separated (2x2 matrix) | [`CODEC-TRANSPORT.md`](CODEC-TRANSPORT.md), [`codec-transport.log`](results/codec-transport.log) |
| `cache-stat` | decodes per row and transports per KiB, from the host's PMU | [`cache-stat.log`](results/cache-stat.log) |
| `client-scaling` | the client itself, on its consumption paths (C++ aggregate, numpy columnar, asyncio, processes), one shared-memory partition per worker | [`client-scaling.log`](results/client-scaling.log), [`client-scaling-nogil.log`](results/client-scaling-nogil.log) |

## Charts: `just charts [target]`

`arrow` - the Arrow read: delivered rates, the ratio against Arrow, the p50 per
read.
`arrow-memory` · `cache` · `cache-budget` · `codec` - the fixed-buffer read, the
result-cache comparisons and the codec x transport matrix.

`ab` - one figure per log, into `docs/assets/` by default.

The look has one home: [`charts/theme.py`](charts/theme.py) sets the sans-serif
type, the thin axes, the dashed grid and the boxed legend, modelled on
[`charts/chart-example.png`](charts/chart-example.png) (the style reference; the
chart itself is not from this repo). Palettes stay with each chart, because there
the color carries the reading.

## Old names, and where they went

| the recipe you may remember | now |
|---|---|
| `just bench` (ADBC + Arrow) | `just rivals adbc` |
| `just pgcache-bench` | `just rivals cache` |
| `just pgcache-memory-bench` | `just rivals cache-budget` |
| `just arrow-memory-bench` | `just rivals arrow-memory` |
| `just decode-cycles`, `-ir`, `-mca` | `just internals decode-cycles` … |
| `just codec-transport` | `just internals codec-transport` |
| `just cache-stat` | `just internals cache-stat` |
| `just charts`, `just codec-charts`, … | `just charts arrow`, `just charts codec`, … |

## Which numbers should I quote?

The committed logs, never a sentence alone. For "how does it compare with the
other stacks", the rival logs; for "why", the internals notebook. The README
carries the headline of each and links the log it came from.
