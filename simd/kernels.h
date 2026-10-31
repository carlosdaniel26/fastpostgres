/* kernels.h — the marshalling kernels the hot path runs, and the table that
 * says which instruction set is answering them.
 *
 * These are pure memory moves: no allocation, no errors, no globals of their
 * own. The extension runs them inside a Postgres backend and the client runs
 * them inside a CPython extension, so the same code has to be valid in both —
 * and a bug in the hand-written assembly is a crash in a database process, not
 * a Python exception. That is why every kernel has a scalar twin that is the
 * reference, and why FASTPOSTGRES_NOSIMD forces the scalar table (the tests
 * compare the two against each other).
 *
 * Only kernels with a real caller live here. The encode side reads its values
 * out of `heap_deform_tuple`'s row-major scratch (stride = ncols), so the
 * packed-array kernels it would want are blocked on de-interleaving that
 * scratch first — see the plan; what ships is what is wired.
 */
#ifndef FASTPOSTGRES_SIMD_KERNELS_H
#define FASTPOSTGRES_SIMD_KERNELS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when any bit is set in the first nbytes, else 0. A column with no nulls
 * skips every per-row null test — the common case. */
typedef int (*fp_nullmap_any_fn)(const uint8_t *bits, size_t nbytes);

/* Copy len bytes (len <= 32). Reads only what it writes, so it needs no slack
 * at either end — the reason it can be called straight into the segment. */
typedef void (*fp_copy_small_fn)(void *dst, const void *src, size_t len);

typedef struct FpKernels {
    const char        *name; /* "scalar" / "avx2" / "avx512" — reported in the ENV block */
    fp_nullmap_any_fn  nullmap_any;
    fp_copy_small_fn   copy_small;
} FpKernels;

extern const FpKernels  fp_kernels_scalar;
extern const FpKernels  fp_kernels_avx2;
extern const FpKernels  fp_kernels_avx512;

/* The table in use; the scalar one until fp_kernels_init() runs. */
extern const FpKernels *fp_kernels;

/* Probe the CPU and choose a table; FASTPOSTGRES_NOSIMD=1 forces the scalar
 * one, which is how the equivalence tests compare the two. */
void fp_kernels_init(void);

/* Assembly cores. They handle whole blocks only — the block size is a
 * contract, and the C wrappers in kernels.c run the tail, so no special case
 * lives in the asm. Defined in kernels_avx2.S / kernels_avx512.S. */
int  fp_nullmap_any_avx2_core(const uint8_t *bits, size_t nbytes);
int  fp_nullmap_any_avx512_core(const uint8_t *bits, size_t nbytes);
void fp_copy_small_avx2(void *dst, const void *src, size_t len);

#ifdef __cplusplus
}
#endif

#endif
