/* kernels.c — the scalar kernels (the reference everything is checked
 * against), the SIMD wrappers that add the tails, and the dispatch. */
#include "kernels.h"

#include <stdlib.h>
#include <string.h>

#include "cpu.h"

/* ---- scalar ------------------------------------------------------------- */

static int scalar_nullmap_any(const uint8_t *bits, size_t nbytes)
{
    size_t i;

    for (i = 0; i < nbytes; i++)
        if (bits[i]) return 1;
    return 0;
}

static void scalar_copy_small(void *dst, const void *src, size_t len)
{
    memcpy(dst, src, len);
}

const FpKernels fp_kernels_scalar = {"scalar", scalar_nullmap_any, scalar_copy_small};

#if defined(__x86_64__) /* the assembly is System V AMD64 only */

/* ---- AVX2 --------------------------------------------------------------- */

#define AVX2_BLOCK 32

static int avx2_nullmap_any(const uint8_t *bits, size_t nbytes)
{
    size_t bulk = nbytes & ~(size_t)(AVX2_BLOCK - 1);

    if (bulk && fp_nullmap_any_avx2_core(bits, bulk)) return 1;
    return scalar_nullmap_any(bits + bulk, nbytes - bulk);
}

const FpKernels fp_kernels_avx2 = {"avx2", avx2_nullmap_any, fp_copy_small_avx2};

/* ---- AVX-512 ------------------------------------------------------------ */

#define AVX512_BLOCK 64

static int avx512_nullmap_any(const uint8_t *bits, size_t nbytes)
{
    size_t bulk = nbytes & ~(size_t)(AVX512_BLOCK - 1);

    if (bulk && fp_nullmap_any_avx512_core(bits, bulk)) return 1;
    return scalar_nullmap_any(bits + bulk, nbytes - bulk);
}

/* copy_small is a plain 16/8/4/2/1-byte ladder — AVX2's wider registers buy
 * nothing at these sizes, so the AVX-512 table reuses it. */
const FpKernels fp_kernels_avx512 = {"avx512", avx512_nullmap_any, fp_copy_small_avx2};

#else /* no hand-written assembly to call: both tables fall back to the scalar */

const FpKernels fp_kernels_avx2 = {"scalar", scalar_nullmap_any, scalar_copy_small};
const FpKernels fp_kernels_avx512 = {"scalar", scalar_nullmap_any, scalar_copy_small};

#endif

/* ---- dispatch ----------------------------------------------------------- */

const FpKernels *fp_kernels = &fp_kernels_scalar;

void fp_kernels_init(void)
{
    const char *off = getenv("FASTPOSTGRES_NOSIMD");
    unsigned    cpu;

    if (off != NULL && off[0] == '1')
    {
        fp_kernels = &fp_kernels_scalar;
        return;
    }

    cpu = fp_cpu_probe();
    if (cpu & FP_CPU_AVX512F)
        fp_kernels = &fp_kernels_avx512;
    else if (cpu & FP_CPU_AVX2)
        fp_kernels = &fp_kernels_avx2;
    else
        fp_kernels = &fp_kernels_scalar;
}
