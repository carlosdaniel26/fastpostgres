# Schema-specialized row-frame deformer

The client decodes the row frame (`FPROWS`) by walking PostgreSQL's own tuple
bytes (`python/src/fastpostgres/native/codec.cpp`, `fp_decode_tuples`). Before
this change the walk already hoisted the kind switch out of the row loop (one
specialised body per column per block of rows). This change specialises the
layout and the branches on top of that.

## What is specialised now

- **Per-batch schema, resolved once.** `AttLayout` is built from the frame's
  column descriptors: wire width (or varlena), tuple alignment, and the column's
  null-bit position (`t_bits` is at a fixed offset; the bitmap width follows
  `ncols`). `RowBlock` keeps each tuple's attribute base (`row + t_hoff`) and all
  offsets are relative to it, so every row can have a different `t_hoff` and the
  same layout still applies.
- **Static attribute offsets.** A fixed-width attribute whose predecessors in
  the block are all fixed-width and null-free has one constant offset, so its
  body reads `base + offset` directly instead of running the
  align/addlength chain per value. The offsets are re-resolved per block
  (cheap: one pass over `ncols`), because a row's nullness only becomes known as
  the frame streams in; the schema part is per batch.
- **Hoisted null test.** The tuple bitmap marks attributes that are *not* null.
  The block ANDs the bitmaps of its rows; a column whose bit survives has no
  null anywhere in the block and gets a body with no per-value null load or
  branch. Tuples without a bitmap contribute nothing (they have no nulls).
- **One varlena-header decode.** The old body asked for the data pointer, the
  data length and then the total size to advance the offset, which decoded the
  same header up to three times. `fp_varatt_size` decodes it once and hands the
  same total size to the offset chain.
- **Branchless header and alignment.** The 1-byte/4-byte varlena choice and
  `att_align_pointer`'s "align only when the first byte is zero" rule are now
  masks/cmovs. On a mixed batch both are data-dependent; on the benchmark shape
  they are not.
- **Block size 16** (was 8): the kind dispatch and the per-block layout pass are
  amortised over twice as many rows while the block still fits in L1.

No wire bytes changed. The row frame, the descriptors and the segment layout are
exactly as before.

## What cannot be specialised

- **Offsets after a varlena.** A text/bytea value's length is per row, so every
  attribute behind the first varlena has a data-dependent offset. Only the
  leading fixed-width / null-free prefix can be constant. The bench shape
  (`id, sku, name, price, stock, active`) makes `id` and `sku` static and leaves
  the other four on the walk.
- **A varlena at an unaligned offset.** `att_align_pointer` aligns a value only
  when its first byte is zero (the 4-byte header form); a 1-byte header is taken
  where it lies. When the schema offset is not already aligned, the actual
  offset depends on the value's own header, so the static path must fall back to
  the per-row rule.
- **A nullable column.** The null test cannot be removed or made truly
  branchless without constructing a value that is then thrown away: Python
  object birth has side effects (allocation, refcounts). It stays a branch, but
  only for columns that actually have a null in the block.
- **The object births.** `PyLong`, `PyFloat`, `PyUnicode` and `PyTuple` are the
  dominant cost of `read_rows`, and the walk is a small share of it. They cannot
  be specialised further without changing object identity, lifetime or the
  CPython ABI, which this driver does not do.

## What the measurement says

`bench/results/team-compilers-ab.log` is the A/B, three interleaved rounds per
shape. On this host the row's run-to-run spread is +/-2-4%, so the honest
reading is:

- bench shape: `read_rows` median **656.0 -> 633.2 cycles/row, -3.5%**
  (5.79M -> 6.00M rows/s equivalent). A direct 8-vs-16 block comparison was
  neutral on this shape, so the gain is the removed per-value null test and the
  static prefix for `id` and `sku`; with a spread this wide it is fair to call
  it 1-3% rather than take the top of the range.
- mixed shape: **739.3 -> 748.5 cycles/row, +1.2%**, i.e. a wash inside the
  spread. Here the branches that actually mispredict are inside
  `PyUnicode_FromStringAndSize`'s UTF-8 decode and the allocator, not in the
  walk; the null test that this change can hoist is a smaller share than the
  births it sits next to.

The deformer work removed is a few instructions per value against a decode
dominated by CPython object births, which is the same conclusion the project's
own meters reached for the earlier loop rewrite: instruction removal from this
loop does not move the row time, because the loop is not where the row's time
goes. The specialisation is kept because the bench shape does improve and
because it removes branches and dependent loads without changing behaviour,
while keeping the per-batch layout in one place for the parts that *can* be
precomputed.
