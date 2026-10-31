/* cpu.c — CPUID + XCR0 feature probe (see cpu.h). */
#include "cpu.h"

#if defined(__x86_64__) || defined(__i386__)

#include <cpuid.h>

static unsigned long long xgetbv0(void)
{
    unsigned eax, edx;

    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return ((unsigned long long)edx << 32) | (unsigned) eax;
}

unsigned fp_cpu_probe(void)
{
    unsigned eax, ebx, ecx, edx;
    unsigned long long xcr0;
    unsigned features = 0;

    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return 0;

    /* OSXSAVE: XGETBV is usable. AVX: the CPU has the YMM state at all. */
    if (!((ecx >> 27) & 1) || !((ecx >> 28) & 1)) return 0;

    xcr0 = xgetbv0();
    if ((xcr0 & 0x6) != 0x6) return 0; /* XMM + YMM state enabled by the OS */

    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx))
    {
        if ((ebx >> 5) & 1) features |= FP_CPU_AVX2;
        /* AVX-512F needs the CPU bit and ZMM state (XCR0 bits 5,6,7). */
        if (((ebx >> 16) & 1) && (xcr0 & 0xe0) == 0xe0) features |= FP_CPU_AVX512F;
    }
    return features;
}

#else /* not x86: the scalar table is all there is */

unsigned fp_cpu_probe(void)
{
    return 0;
}

#endif
