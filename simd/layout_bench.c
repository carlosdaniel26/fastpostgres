/* layout_bench.c — does de-interleaving the deform scratch (R3) pay?
 *
 * columnar.c's fixed-width encode reads heap_deform_tuple's ROW-MAJOR scratch,
 * vals[r * ncols + c]. For one column that is one element every ncols slots, so
 * a column pass sweeps the whole nrows*ncols scratch: the array is walked once
 * per column. The alternative is to transpose the scratch to column-major once,
 * after which every column pass is contiguous.
 *
 * Three variants produce byte-identical output and run the SAME inner loop —
 * only the source stride differs, so the numbers isolate the layout:
 *
 *   A  strided, as today
 *   B  transpose first, then contiguous
 *   C  contiguous source, no transpose (upper bound: what a column-major
 *      deform would give for free)
 *   T  the transpose on its own, to price it
 *
 * Caveats, so the numbers are not over-read: this is built -O3 with no -march
 * (the extension is compiled with Postgres's flags, so the real loop is
 * baseline x86-64 too), and the transpose is a blocked scalar 8x8 — a
 * hand-written AVX-512 transpose would be faster, so B's cost here is a ceiling.
 *
 * Run with `just kernels`.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef uint64_t Slot; /* a Datum: 8 bytes */

#define KIND_BOOL   1
#define KIND_INT16  6
#define KIND_INT32  2
#define KIND_INT64  3
#define KIND_FLOAT8 4

#define REPEATS 5

/* The bench table's shape: a realistic mix of widths, no conversions. */
static const int KINDS[16] = {KIND_INT32, KIND_INT64, KIND_FLOAT8, KIND_BOOL, KIND_INT32, KIND_INT64,
                              KIND_FLOAT8, KIND_INT16, KIND_INT32, KIND_INT64, KIND_FLOAT8, KIND_BOOL,
                              KIND_INT32, KIND_INT64, KIND_FLOAT8, KIND_INT16};
static const int WIDTH[16] = {4, 8, 8, 1, 4, 8, 8, 2, 4, 8, 8, 1, 4, 8, 8, 2};

static double
now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* The encode inner loop. stride is in slots: ncols for the row-major source,
 * 1 for a column-major one. */
static void
encode_column(char *dst, const Slot *base, size_t stride, uint64_t n, int kind)
{
    uint64_t r;

    switch (kind)
    {
        case KIND_BOOL: {
            uint8_t *d = (uint8_t *)dst;
            for (r = 0; r < n; r++) d[r] = (uint8_t)(base[r * stride] != 0);
            break;
        }
        case KIND_INT16: {
            int16_t *d = (int16_t *)dst;
            for (r = 0; r < n; r++) d[r] = (int16_t)base[r * stride];
            break;
        }
        case KIND_INT32: {
            int32_t *d = (int32_t *)dst;
            for (r = 0; r < n; r++) d[r] = (int32_t)base[r * stride];
            break;
        }
        default: { /* int64 / float8: the slot already is the value */
            uint64_t *d = (uint64_t *)dst;
            for (r = 0; r < n; r++) d[r] = base[r * stride];
            break;
        }
    }
}

/* src (row-major, ncols per row) -> dst (column-major, nrows per column),
 * in 8x8 tiles so both sides stay in cache. */
static void
transpose(const Slot *src, Slot *dst, uint64_t nrows, int ncols)
{
    int      c0, c, r;
    uint64_t r0;

    for (r0 = 0; r0 < nrows; r0 += 8)
    {
        uint64_t rlim = nrows - r0 < 8 ? nrows - r0 : 8;

        for (c0 = 0; c0 < ncols; c0 += 8)
        {
            int clim = ncols - c0 < 8 ? ncols - c0 : 8;

            for (r = 0; r < (int)rlim; r++)
                for (c = 0; c < clim; c++)
                    dst[(uint64_t)(c0 + c) * nrows + (r0 + r)] = src[(r0 + r) * (uint64_t)ncols + (c0 + c)];
        }
    }
}

static uint64_t
out_bytes(int ncols, uint64_t nrows)
{
    uint64_t total = 0;
    int      c;

    for (c = 0; c < ncols; c++) total += (uint64_t)WIDTH[c] * nrows;
    return total;
}

static void
run(uint64_t nrows, int ncols)
{
    Slot  *vals = malloc(nrows * ncols * sizeof(Slot));
    Slot  *cmajor = malloc(nrows * ncols * sizeof(Slot));
    Slot  *scratch = malloc(nrows * ncols * sizeof(Slot));
    char **outA = malloc(ncols * sizeof(char *));
    char **outB = malloc(ncols * sizeof(char *));
    char **outC = malloc(ncols * sizeof(char *));
    double tA, tB, tC, tT, t;
    uint64_t i, total = nrows * (uint64_t)ncols;
    int  c, rep;
    int  bad = 0;

    for (i = 0; i < total; i++) vals[i] = i * 2654435761u;
    for (c = 0; c < ncols; c++)
    {
        outA[c] = malloc(nrows * 8);
        outB[c] = malloc(nrows * 8);
        outC[c] = malloc(nrows * 8);
    }
    transpose(vals, cmajor, nrows, ncols); /* variant C's input, built up front */

    tA = tB = tC = tT = 1e9;
    for (rep = 0; rep < REPEATS; rep++)
    {
        double dt;

        t = now_s();
        for (c = 0; c < ncols; c++) encode_column(outA[c], vals + c, (size_t)ncols, nrows, KINDS[c]);
        dt = now_s() - t;
        if (dt < tA) tA = dt;

        t = now_s();
        transpose(vals, scratch, nrows, ncols);
        for (c = 0; c < ncols; c++) encode_column(outB[c], scratch + (uint64_t)c * nrows, 1, nrows, KINDS[c]);
        dt = now_s() - t;
        if (dt < tB) tB = dt;

        t = now_s();
        for (c = 0; c < ncols; c++) encode_column(outC[c], cmajor + (uint64_t)c * nrows, 1, nrows, KINDS[c]);
        dt = now_s() - t;
        if (dt < tC) tC = dt;

        t = now_s();
        transpose(vals, scratch, nrows, ncols);
        dt = now_s() - t;
        if (dt < tT) tT = dt;
    }

    for (c = 0; c < ncols; c++)
    {
        if (memcmp(outA[c], outB[c], nrows * 8) != 0 || memcmp(outA[c], outC[c], nrows * 8) != 0) bad = 1;
    }

    printf("  ncols=%-2d %6s rows   A %7.1f   B %7.1f   C %7.1f   T %7.1f  M values/s   "
           "B/A %.2fx  C/A %.2fx\n",
           ncols, nrows >= 1000000 ? "1M" : "100K", total / tA / 1e6, total / tB / 1e6, total / tC / 1e6,
           total / tT / 1e6, tA / tB, tA / tC);
    if (bad) printf("    MISMATCH: the variants disagree\n");
    printf("    scratch %u MiB, packed output %u MiB\n", (unsigned)(nrows * ncols * 8 >> 20),
           (unsigned)(out_bytes(ncols, nrows) >> 20));

    for (c = 0; c < ncols; c++)
    {
        free(outA[c]);
        free(outB[c]);
        free(outC[c]);
    }
    free(outA);
    free(outB);
    free(outC);
    free(vals);
    free(cmajor);
    free(scratch);
}

int
main(void)
{
    printf("layout bench — de-interleave the deform scratch?\n");
    printf("  A strided (today)   B transpose+contiguous   C contiguous source (bound)   T transpose alone\n");

    run(100000, 8);
    run(1000000, 8);
    run(1000000, 16);
    return 0;
}
