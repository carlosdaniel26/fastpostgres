# regipc vector-call probe — is ABI v3 worth it?

This directory is a **feasibility experiment, not a product change**: a second,
separately named module (`regipcvec`, its own `/dev/regipcvec`) implements
`REGIPC_IOC_CALL_V`, the ABI 2 call plus a transfer of the caller's vector state,
and `fpvec.c` measures what that costs. The loaded ABI 2 `/dev/regipc` is left
untouched. Nothing here is committed.

*Status: all three code paths build clean (plain, vec, bulk). The module run is
**pending** because this machine has no cached `sudo` credential (`sudo -n`
fails with "a password is required"); per the experimental rules nothing was
prompted and nothing was worked around. The userspace proxy — the FP
save/restore floor — **was** measured, and it already answers most of the
question; its numbers and the projections built on them are below, with the
exact two commands to finish the module table by hand. The copy path
(`CALL_B`, pointer + length through `copy_from_user`) is built and wired into
the same sweep; its session is pending on this machine for the same reason —
its section carries the two commands and the decision rule it will settle.*

---

## What was built

| file | what it is |
|---|---|
| `regipcvec.c` | the module: ioctls 1..5 are the ABI 2 contract unchanged (`REGISTER`, `PAIR`, `CALL`, `STATS`, `QUIT`), plus `REGIPC_IOC_CALL_V = _IO('c', 6)`, `REGIPC_IOC_VINFO = _IOR('c', 7, ...)` and `REGIPC_IOC_CALL_B = _IO('c', 8)`. `CALL_V` runs the same leaf bookkeeping as `CALL`, then `kernel_fpu_begin()` saves the caller's live vector state into its task's fpstate and `width` bytes of it are copied into the callee's pinned buffer, then `kernel_fpu_end()`. `CALL_B` runs the same bookkeeping, then `copy_from_user()`s a caller-supplied buffer into the same arena page; no FPU state is touched. Both publish the leaf counter with a release store after their bytes are in place. |
| `Makefile` | kbuild (`KDIR ?= /lib/modules/$(uname -r)/build`) + the userspace program + `load`/`unload`/`sweep`/`bulk`/`bulk-sweep`/`plain`/`xsave`/`selftest` targets. `load`/`unload`/`sweep`/`bulk-sweep` use `sudo -n` and fail with the hand commands; they never prompt. |
| `fpvec.c` | the two-process measurement program: parent = producer/caller, forked child = callee (no threads). Modes `--mode plain`, `--mode vec --width N`, `--mode bulk --width N [--cold]`, `--mode xsave`, `--selftest`. One JSON object per run on stdout. |
| `chart.py` | the figure: p50 per call, calls/s and GB/s for the vector channel and the copy channel (hot and cold), all from the jsonl records. |
| `table.py` | prints the two README tables from `sweep-bulk.jsonl` — medians of the per-run fields, nothing typed by hand. |
| `README.md` | this file. |

### The two design points worth knowing

1. **Two pages per callee.** The vector payload cannot share a page with the
   leaf struct: `run_leaf()` rewrites `magic/calls/values/checksum` on every
   call, so a copy over those bytes would break the next call's magic check.
   The callee therefore registers one page-aligned 8192-byte region: page 0 is
   the leaf struct (byte-for-byte the ABI 2 layout), page 1 is the vector arena
   — exactly one page, so up to 4096 B (the spec's "the callee's pinned buffer
   (one page, so up to 4096 B)" holds for the arena; the deviation is that the
   registration is two pages, not one).
2. **Width is a module parameter, not an ioctl argument.** `width ∈ {0, 256,
   1024, 4096}`; `width=0` runs the FP save/restore and copies nothing, which is
   the point that isolates the FP cost from the per-byte copy cost. Changing it
   is `rmmod` + `insmod width=N`. The copy itself is `min(width, fpstate->size)`
   bytes of the saved image, and if `width` exceeds the real image (AVX-512
   XSAVES images here are **2440 bytes**, not 4 KB) the rest of the arena is
   zero-filled, so the callee always receives exactly `width` bytes.

`VINFO` reports `{width, fpstate_size, real_bytes, xfeatures, format, flags}` so
the program never guesses the image format (XSAVE vs XSAVES vs FXSAVE) or the
payload size, and `fpvec --mode vec --width N` refuses to run if the module is
loaded with another width.

## Exact commands

Build (done, clean):

```sh
make -C extension/transport/regipc/vector            # module + fpvec
make -C extension/transport/regipc/vector module W=1 # no warnings
```

Load / unload (this machine: `sudo -n` is **not** cached, so `make load` exits
with these two commands and does not prompt):

```sh
sudo insmod /home/carlos/Projects/fastpostgres/extension/transport/regipc/vector/regipcvec.ko width=1024 mode=0666
sudo rmmod regipcvec
```

`mode=0666` matters: the device is created root-owned and the measurement
program runs unprivileged.

The sweep, once loaded (or via `make -C ... sweep`, which reloads the module for
each width and traps an unload on exit):

```sh
make -C extension/transport/regipc/vector unload
for w in 0 256 1024 4096; do
  sudo insmod .../regipcvec.ko width=$w mode=0666
  ./extension/transport/regipc/vector/fpvec --mode vec --width $w --reps 3 --count 200000
  sudo rmmod regipcvec
done
./extension/transport/regipc/vector/fpvec --mode plain --reps 3 --count 200000
```

Nothing below needed root:

```sh
./extension/transport/regipc/vector/fpvec --mode xsave --reps 5   # measured, see below
./extension/transport/regipc/vector/fpvec --selftest              # cannot run: /dev/regipcvec is absent
```

`--selftest` fails with `FAIL plain: /dev/regipcvec: No such file or directory
(is the regipcvec module loaded?)` until the module is loaded — the selftest is
meaningless without the device and was not run. (The copy-path session is at the
bottom of this file: `sudo insmod ... width=1024 mode=0666` then
`make -C ... bulk-sweep`, which writes `sweep-bulk.jsonl`.)

## Measured now: the userspace proxy (the floor of the FP cost)

`--mode xsave` runs the same instruction pair the kernel's fpstate uses
(`xsave64`/`xrstor64`; the kernel saves with XSAVES and restores with XRSTORS,
which userspace cannot execute), over the full AVX-512 user state: three
10⁶-iteration loops per rep (save alone, restore alone, save+restore), 5 reps:

```json
{"mode":"xsave","rep":1,"reps":5,"mask":743,"bytes":2440,"iterations":1000000,"ns_save":52.75,"ns_restore":120.67,"ns_save_restore":170.87}
{"mode":"xsave","rep":2,"reps":5,"mask":743,"bytes":2440,"iterations":1000000,"ns_save":51.70,"ns_restore":119.93,"ns_save_restore":169.78}
{"mode":"xsave","rep":3,"reps":5,"mask":743,"bytes":2440,"iterations":1000000,"ns_save":52.09,"ns_restore":120.21,"ns_save_restore":170.07}
{"mode":"xsave","rep":4,"reps":5,"mask":743,"bytes":2440,"iterations":1000000,"ns_save":51.57,"ns_restore":119.94,"ns_save_restore":171.71}
{"mode":"xsave","rep":5,"reps":5,"mask":743,"bytes":2440,"iterations":1000000,"ns_save":51.73,"ns_restore":120.32,"ns_save_restore":170.31}
```

| operation | median | per call |
|---|---|---|
| `xsave64` of the full AVX-512 user state (mask 0x2e7, 2440 B) | 52.09 ns | one per V call |
| `xrstor64` of the same | 120.21 ns | one per V call (the kernel restores on return to userspace) |
| **save + restore** | **170.31 ns** | **the FP floor a V call pays** |

The plain ABI 2 call never touches the FPU, so it pays none of this: the V call
adds ≥ 170 ns to the established 0.521 us p50, i.e. **+33 % before a single byte
is copied**. The kernel's XSAVES/XRSTORS pair cannot be cheaper than the
userspace XSAVE/XRSTOR pair by more than a little (same instruction family,
same 2440 bytes, plus `preempt_disable` and the MXCSR reset).

## The copy side, also measured rootless (hot caches, userspace memcpy proxy)

```sh
memcpy 256 B: 2.87 ns (89.3 GB/s)
memcpy 1024 B: 9.55 ns (107.2 GB/s)
memcpy 2440 B: 22.82 ns (106.9 GB/s)
memcpy 4096 B: 35.51 ns (115.3 GB/s)
```

The module's `memcpy` + zero-fill is the same kernel `memcpy()` the kernel
always uses; at these sizes it should land within a small factor of these
numbers (source = the fpstate just written by XSAVES, destination = the arena
page just written by the previous call, both cache-hot). **The copy is not the
cost; the FP save/restore is.**

## The module table (pending: needs the two hand commands)

`fpvec` emits one line per run, e.g.

```json
{"mode":"vec","width":1024,"real_bytes":1024,"fpstate_size":2440,"format":3,"rep":1,"reps":3,
 "calls":200000,"warmup":1000,"errors":0,"bytes_per_call":1024,"seconds":0.15...,"calls_per_second":...,
 "gb_per_second":...,"us_per_call_p50":...,"us_per_call_p99":...,"gaps":...,
 "producer_cpu_s":...,"consumer_cpu_s":...,"child_cpu_us":...,"checksum":...,"received_checksum":...,
 "child_checksum":...,"leaf_calls":201000,"leaf_checksum":...,"verified":true,...}
```

The record proves itself: `leaf_calls == warmup + calls` exactly (every call
once, in order), `leaf_checksum == gpr_checksum` (the module's own FNV over
x0..x8, in the leaf's `checksum` field), and in vec mode the caller's expected
fold, the parent's read of the arena and the callee's own fold must all be
equal, over the register bytes that lie inside the copied prefix (XMM low halves
at 160+16*i; YMM-high/ZMM-high/Hi16-ZMM at the layout the module reports via
`VINFO`), plus the zero-fill tail. `verified` false means bytes did not arrive —
the numbers are then not to be used.

Projection from the proxies above, clearly labelled as such (base = measured
0.521 us plain call + measured 170.3 ns FP floor + hot-memcpy copy):

| width | bytes per call | real bytes copied | projected p50 | projected calls/s | projected GB/s |
|---|---|---|---|---|---|
| 0 | 0 | 0 | ~0.69 us | ~1.45 M | — |
| 256 | 256 | 256 | ~0.70 us | ~1.43 M | ~0.37 |
| 1024 | 1024 | 1024 | ~0.70 us | ~1.42 M | ~1.46 |
| 4096 | 4096 | 2440 + 1656 zeros | ~0.73 us | ~1.37 M | ~5.6 (arena) / ~3.3 (real bytes) |

The real numbers from the module run are expected slightly worse: kernel XSAVES
+ XRSTORS rather than XSAVE/XRSTOR, a full `kernel_fpu_begin/end`, a copy whose
destination is large enough to miss L1 every so often, and the per-call ZMM
loads/state-save that come with putting AVX-512 on the hot path (Zen 4's
AVX-512 clock behaviour is mild, but the traffic is not free).

## Verdict

**Does the bandwidth exist?** Yes, up to a point. At the measured FP floor the
V call lands around 0.7 us and carries 256 B-4 KB, i.e. **~0.37 GB/s at the
256-byte point, ~1.5 GB/s at 1 KB, and ~3.3-5.6 GB/s at its best (a full
arena)** — about 3×, 12× and 28-47× the end-to-end scan's ~118 MB/s, and at
best the same order as one core's memcpy.

**What does it cost?** +170 ns per call for the FP pair (a floor; the kernel
adds its own work on top) — **+33 % on the 0.521 us plain call at width 0**,
where the payload is nothing at all. The copy itself is noise (tens of ns).
The FP save/restore, not the bytes, is the ABI v3 tax, and it is paid on every
call including the empty ones.

**Should the graph ride it?** No, for two independent reasons:

1. **The segment path's transport is not the bottleneck — and the vector
   channel does not add bandwidth.** The scan's ~118 MB/s is what the *client*
   leaves after decoding the rows. The vector channel's ceiling is 0.4-5.6 GB/s,
   i.e. at best one core's memcpy rate — the same order as the raw publish it
   would replace. It buys no headroom, and it adds a per-call FP tax even to
   the calls that carry nothing.
2. **The payload is three orders of magnitude too big.** The scan moves ~47 MB
   in 10 payload batches ≈ 4.7 MB per batch. A V call can carry at most 4096 B
   (2440 B of real AVX-512 state here). One batch would be 1000+ calls, each
   paying the FP save/restore — about a millisecond of pure FP overhead per
   batch for bytes the ring's data segment already moves at full memcpy speed.

The honest niche is the opposite end: a *small* payload (≤ ~1 KB) where the
doorbell and the payload are the same call and there is no decode to pay. Even
there the measured floor says the V call costs +33-40 % per call versus ABI 2,
and reaches only ~0.4-1.5 GB/s.

**What would it take to make it real, and is that worth an ABI v3?**

- *Shape.* `CALL_V` as ioctl 6 with the ABI 2 register contract untouched; the
  callee's registration extended to page 0 leaf + page 1 arena (or an arena
  address passed at `PAIR` time); width negotiated at registration or carried
  in a spare carrier; a `VINFO`-style ioctl so userspace can reason about the
  image's format and size. The proposal measured here.
- *Caller-side asm constraint (the sharp edge).* The vector load and the
  `syscall` must be one asm block (all XMM/ZMM are caller-saved; a library call
  between load and syscall destroys the payload — observed while writing the
  probes: one `memset` was enough). GCC will not accept `zmm16..zmm31` clobbers
  without AVX-512 compile flags ("the register 'zmm31' cannot be clobbered in
  'asm' for the current target", GCC 16.2 on this box), so the practical route
  is the technique's own: a `.S` stub, or a translation unit built with
  `-mgeneral-regs-only` (what `fpvec.c` does). The blob pointer must live in a
  GPR, and RDX is already the ioctl's argument word and carrier w[0].
- *Kernel exports.* On this kernel (`7.2.8-9-omarchy`), `kernel_fpu_begin_mask`
  and `kernel_fpu_end` are `EXPORT_SYMBOL_GPL` (verified in `Module.symvers`),
  so a GPL module can do this today. The fpstate itself is reached through
  `x86_task_fpu(current)` — a **macro that hard-codes the task_struct layout**
  (this kernel moved `struct fpu` out of `thread_struct` to just after the
  task_struct; `switch_fpu_return` uses `task+0x1840`). That is an ABI hazard a
  real v3 must not inherit: it needs an exported accessor, not a macro.
  `save_fpregs_to_fpstate()`/`os_xsave()` are not exported and are not needed
  (`kernel_fpu_begin()` performs the save).
- *The cheaper design the measurements point at.* The kernel-side save/restore
  is the cost, and it is avoidable: the **caller can XSAVE its own state to a
  shared page in 52 ns** (measured) and then issue a payload-free call, or pass
  a pointer/length so the module copies from the caller's staging buffer (a copy
  either way, no kernel FP). A v3 that keeps `kernel_fpu_begin()` pays ~170 ns
  per call for something the caller can do for ~50 ns.

**Bottom line.** The vector channel reaches single-digit GB/s at ~1.4 M calls/s
— real bandwidth, but no more than the same order as the segment path's raw
publish, and irrelevant to a workload whose 118 MB/s is decode-bound and whose
batches are megabytes, not kilobytes. At +33-40 % per call before any payload,
**an ABI v3 built this way is not worth it for the graph's shape**; the
small-payload niche does not justify a new ABI either, because the existing
segment plus a doorbell is already as fast per byte, and the measured FP tax
buys nothing the ring does not already have.

## Reproducing the pending module run

```sh
cd /home/carlos/Projects/fastpostgres

# 1. what the machine says about the FPU state (measured already)
make -C extension/transport/regipc/vector xsave

# 2. load the probe and run the selftest, then the sweep, then unload
sudo insmod extension/transport/regipc/vector/regipcvec.ko width=1024 mode=0666
./extension/transport/regipc/vector/fpvec --selftest --width 1024
make -C extension/transport/regipc/vector sweep        # unloads itself
sudo rmmod regipcvec                                   # if the sweep was run by hand
```

## Measured: the sweep, on this machine

`make sweep`, 3 reps per width, one module reload per point, every run verified
(the child received exactly what the caller sent, the leaf counted every call).

| width | real bytes | p50 | p99 | calls/s | arena GB/s |
|---|---|---|---|---|---|
| 0 (FP tax only) | 0 | 0.651 µs | 0.822 µs | 1.449 M | - |
| 256 | 256 | 0.671 µs | 0.691 µs | 1.434 M | 0.367 |
| 1024 | 1024 | 0.672 µs | 0.752 µs | 1.416 M | 1.450 |
| 4096 | 2456 | 0.711 µs | 0.722 µs | 1.355 M | 5.548 |

Against the plain ABI 2 call (0.521 µs), the fit is:

- **intercept: +130 ns per call (+25%), carrying nothing** - that is
  `kernel_fpu_begin/end`, and it is paid on every call whatever the width;
- **slope: +60 ns for 2456 bytes** (~24 ns/KiB, 2.6x a hot userspace copy) - the
  copy is cheap; the tax is not;
- at the widest point the channel moves 3.3 GB/s of *real* state (5.5 GB/s of
  arena), which is ~26x the 72 B GPR channel's 125 MB/s.

The verdict, now measured rather than projected: the width works, and it still
does not pay. A real batch is ~4.7 MB, i.e. ~1900 calls of 2456 B; at 0.711 µs
each that is 1.36 ms of calls per batch, ~3.4% added to a 40 ms batch, on top of
the payload still having to be built by the worker and copied here page by page -
where the segment path copies it once and the client reads it in place. And the
per-call tax is on every call, including the ones that carry nothing.

What the numbers point at instead: the caller can `xsave` its own state into a
shared page in ~52 ns and issue a payload-free call, avoiding the kernel's 130 ns
entirely - which is the architecture fastpostgres already has (the segment moves
the bytes at memcpy speed, the call moves the notification at 0.5 µs).

---

# The copy path (`CALL_B`): pointer + length instead of the register file

## The hypothesis this section tests

The vector path widens the register file: measured, it pays **+130 ns of FP
save/restore per call** (2456 B of real AVX-512 state, 3.3 GB/s of real state /
5.5 GB/s of arena at the widest point). The hypothesis:

> Instead of widening the register file, the caller passes a **pointer and a
> length** in the carriers and the ioctl handler does `copy_from_user()` of that
> buffer straight into the callee's pinned page. No FP save/restore (so no
> 130 ns tax), no asm stub needed to hold values in XMM, any length up to the
> page, and the caller writes ordinary C. **Estimated ~4-5 GB/s at 4 KB, which
> would beat the vector path.**

## What was built

`REGIPC_IOC_CALL_B = _IO('c', 8)`: the ABI 2 leaf bookkeeping unchanged, then a
copy. The nine carriers are a descriptor:

| carrier | field | meaning |
|---|---|---|
| w[0] | `flags` | reserved, must be 0 (nonzero → `-EINVAL`) |
| w[1] | `user_ptr` | the caller's source buffer (plain C memory) |
| w[2] | `length` | bytes to copy, 0..`PAGE_SIZE` |
| w[3] | `seq` | caller's sequence number (carried in the leaf's `values[3]`) |
| w[4] | `dst_off` | destination offset inside the callee's arena page |
| w[5..8] | reserved | must be 0, else `-EINVAL` |

`length + dst_off <= PAGE_SIZE` (the arena is exactly one page, so **the biggest
length the module accepts is 4096 B**; 4097 is refused before anything is
touched, and `fpvec` probes that on every bulk run — the `limits_ok` field).
`access_ok` is implicit in `copy_from_user()`; a failed copy returns `-EFAULT`,
and because the copy happens **before** the leaf runs, a failed copy does not
move the leaf counter at all — the callee never observes a published call whose
payload is incomplete. Ordering is copy first, then a **release store** of the
leaf counter (the leaf's `values`/`checksum` are written before it too). The
handler runs in the caller's own process context at process level, with no
spinlock held, so the copy may fault and sleep, which an ioctl may do.

No FPU: `CALL_B` never calls `kernel_fpu_begin()`. The caller side is the same
`regipc_handoff()` the plain ABI 2 call uses — no vector register touches, no
`-mgeneral-regs-only` stub, no `zmm` clobbers.

`fpvec --mode bulk --width N [--cold]` sends an N-byte buffer through it with
the same JSON record as the other modes plus
`cold`/`cold_bytes`/`cold_stride`/`page_bytes`/`child_seq_match`/`limits_ok`.
The verification chains are the vector ones: the caller's expected fold, the
parent's fold of the shared arena, and the **callee's own fold of what it
received** must all agree; the leaf counter, its checksum and `values[3]` (the
`seq`) are checked too. `--cold` allocates a **256 MiB** mapping (8× this
machine's 32 MiB L3), fills it with a repeating 64-byte period and walks it with
a **64 KiB stride**, so every call reads addresses that have long been evicted;
every slice holds the same bytes, so one expected fold covers every call — and
because a failed copy returns `-EFAULT` without publishing (never a partial
one) and the leaf counter must equal `warmup + calls` exactly, a verified run
means every call copied and the last one is byte-checked through the arena.

## Exact commands

Build (done, clean — module with `W=1`, no warnings):

```sh
make -C extension/transport/regipc/vector            # module + fpvec
make -C extension/transport/regipc/vector module W=1
```

The session (this machine: `sudo -n` is **not** cached, so nothing below was
run and the two tables are pending):

```sh
sudo insmod /home/carlos/Projects/fastpostgres/extension/transport/regipc/vector/regipcvec.ko width=1024 mode=0666
make -C /home/carlos/Projects/fastpostgres/extension/transport/regipc/vector bulk-sweep
```

`bulk-sweep` is one session, one file: the bulk table first (256/1024/4096 B,
hot then cold, 3 reps each — `CALL_B`'s length is per call, so a single module
load serves all of it), then the vector points (0/256/1024/4096) reloaded per
width **with the same binary**, so the two tables are comparable. It traps an
`rmmod` on exit and writes every raw JSON line to `sweep-bulk.jsonl`. It uses
`sudo -n` throughout and never prompts; `make bulk` runs just the copy table
against an already-loaded module without using privileges. The module is the
variant's own `/dev/regipcvec`; the loaded ABI 2 `/dev/regipc` is never touched.

After the session, the two tables are one command away — medians of the
per-run fields, nothing typed:

```sh
python3 extension/transport/regipc/vector/table.py   # prints both markdown tables from sweep-bulk.jsonl
python3 extension/transport/regipc/vector/chart.py   # the figure, also from sweep-bulk.jsonl
```

## Status: the numbers are pending on this machine

The build is complete and the sweep is wired, but `sudo -n` is not cached here
("a password is required"), and per the experimental rules nothing was prompted
and nothing was worked around. The two tables below are the ones `bulk-sweep`
fills; their raw lines will live in **`sweep-bulk.jsonl`** beside `sweep.jsonl`
(which is where the vector table further up came from). Table 2 is a
same-session re-run of the vector points — the table further up is from an
earlier session and is *not* the comparison basis.

### Table 1 — the copy channel, from `sweep-bulk.jsonl` (mode `bulk`)

| length | p50 hot | p50 cold | calls/s hot | calls/s cold | GB/s hot | GB/s cold | verified |
|---|---|---|---|---|---|---|---|
| 256 B | pending | pending | pending | pending | pending | pending | pending |
| 1024 B | pending | pending | pending | pending | pending | pending | pending |
| 4096 B (the largest the one-page arena accepts) | pending | pending | pending | pending | pending | pending | pending |

### Table 2 — the vector channel re-run in the same session, from `sweep-bulk.jsonl` (mode `vec`)

| width | real bytes | p50 | p99 | calls/s | arena GB/s | verified |
|---|---|---|---|---|---|---|
| 0 (FP tax only) | 0 | pending | pending | pending | - | pending |
| 256 | 256 | pending | pending | pending | pending | pending |
| 1024 | 1024 | pending | pending | pending | pending | pending |
| 4096 | 2456 | pending | pending | pending | pending | pending |

### The chart

`chart.py` was **extended into one figure** (not a second chart): it now draws
three panels — p50 per call, calls/s, GB/s — with the vector channel and the
copy channel (hot and cold) grouped on the same width axis, so the comparison
is read left to right at each width instead of flipping between figures. The
GB/s panel also keeps the vector channel's *real AVX-512 bytes* series (arena
rate scaled by `real_bytes/width`, all from the record), because that is the
number the copy path has to beat. Everything is a median of the per-run JSON
fields from `sweep-bulk.jsonl` (default: `sweep-bulk.jsonl` if it exists, else
`sweep.jsonl`); no number is typed into the code. `make chart` writes
`regipc-copy-vs-vector.png` **inside this experiment's directory** (the target
deliberately does not write into `docs/`).

## How the two tables answer the hypothesis

- **Per-call cost.** At equal width, `vec p50 − bulk p50` is the FP tax the
  copy path removes. The vector fit puts that intercept at +130 ns **whatever
  the width** (it is paid even at width 0, where it carries nothing); the copy
  path's intercept is the plain call plus `copy_from_user` entry work, and its
  slope is the copy itself. The userspace `memcpy` proxy already measured the
  floor of the byte cost: 2.87 ns at 256 B, 9.55 ns at 1024 B, 35.51 ns at
  4096 B (hot); `copy_from_user` is the same kernel copy plus fault-safety
  entry, so the bulk p50 at 4096 should sit near `0.521 µs + copy`, not near
  the vec 0.711 µs.
- **Bandwidth.** The copy path's payload is **all real bytes** (no zero fill),
  so its GB/s at 4096 B is directly comparable to the vector channel's
  **3.33 GB/s of real AVX-512 state** (5.548 GB/s of arena). **The copy path
  beats the vector path if Table 1's 4096 B hot GB/s is above 3.33** — and the
  hypothesis predicts ~4-5 GB/s.
- **The xsave-vector comparison.** The FP save/restore the vector path pays is
  measured twice: the module fit (+130 ns/call) and the rootless userspace
  proxy (`xsave64` 52 ns + `xrstor64` 120 ns = **170 ns** for mask 0x2e7,
  2440 B). `CALL_B` pays **zero** of it: there is no `kernel_fpu_begin`, no
  task fpstate, no `x86_task_fpu()` macro whose hard-coded `task_struct` layout
  is an ABI hazard. Its fixed costs are validation plus the copy.
- **What the cold column is for.** A caller that just produced a payload owns
  it in cache, so the hot column is the realistic one; the cold column is the
  honest upper bound for a large or streaming source (256 MiB walked with a
  64 KiB stride — every slice from DRAM). If even the cold 4096 B point stays
  above 3.33 GB/s, the vector design has no bandwidth argument left.

## What it would mean for an ABI v3

If the tables come back as the hypothesis predicts, the shape to standardize is
**`(user_ptr, length, dst_off)` copy**, not FP state:

- no `kernel_fpu_begin/end` — the 130 ns tax disappears, and with it the
  dependency on the task fpstate layout (`x86_task_fpu()` is a macro that
  hard-codes `task_struct`, an ABI hazard a real v3 must not inherit);
- no asm stub, no `-mgeneral-regs-only` translation unit, no `zmm` clobber
  constraints — the caller is ordinary C, and the payload can be any buffer the
  caller already has (a batch page, a wire buffer), not just registers the
  compiler happened to leave live;
- any length up to the page, not the 2456 B of real AVX-512 state;
- ordering and error handling are simple and testable: validate, copy
  (`-EFAULT` on fault), then publish with a release store.

The FP state design's one advantage is that the payload is already in the
registers (no source buffer to write), which matters only when the bytes exist
only in registers — for a database's batches they exist in memory anyway. On
the evidence available the copy path should beat the vector path's *real-state*
rate by construction; whether it clears 3.33 GB/s at 4096 B, and what each
length actually costs, is exactly what the two pending tables will read. Until
they exist, **the verdict on the copy path is PENDING on this machine**; the
two commands above produce it.

**Provisional decision rule** (what the measured tables will be read against):

1. bulk p50 at 256/1024/4096 vs vec p50 at the same widths — the difference is
   the FP tax removed, floor +130 ns;
2. bulk GB/s at 4096 (hot) vs **3.33 GB/s** (vec real state) and vs
   **5.55 GB/s** (vec arena) — above which the copy path is the better
   transport, with no FPU and no asm;
3. bulk GB/s cold at 4096 vs the same thresholds — whether the win survives a
   cache-cold source.


