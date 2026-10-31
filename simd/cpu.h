/* cpu.h — what the CPU (and the OS) will actually let us run.
 *
 * CPUID alone is not enough for the wide instructions: without XCR0 saying the
 * OS saves the YMM/ZMM state, the first AVX instruction traps. So the probe
 * checks both, the way ffmpeg's cpu detection does, and the dispatch table is
 * filled in once from the answer (kernels.c).
 */
#ifndef FASTPOSTGRES_SIMD_CPU_H
#define FASTPOSTGRES_SIMD_CPU_H

#define FP_CPU_AVX2    (1u << 0)
#define FP_CPU_AVX512F (1u << 1)

#ifdef __cplusplus
extern "C" {
#endif

/* Bitmask of FP_CPU_* this CPU + OS can execute; 0 on anything else. */
unsigned fp_cpu_probe(void);

#ifdef __cplusplus
}
#endif

#endif
