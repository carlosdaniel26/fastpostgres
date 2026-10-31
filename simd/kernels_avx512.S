/* kernels_avx512.S — hand-written AVX-512 kernels.
 *
 * Only reached when fp_cpu_probe() saw both the CPU feature and the OS's ZMM
 * state; the null test's block size is 64, matching one zmm.
 */
#if defined(__x86_64__)

	.text

/* int fp_nullmap_any_avx512_core(const uint8_t *bits, size_t nbytes)
 *   rdi = bits, rsi = nbytes (a multiple of 64; 0 is a multiple of 64).
 *   Returns 1 if any byte is set. vptestmq turns the per-qword test into a mask
 *   register, so the loop needs no branch per block. Like the AVX2 version it
 *   tests the data against itself: (a & b) against a zero operand is always
 *   zero. */
	.globl	fp_nullmap_any_avx512_core
	.type	fp_nullmap_any_avx512_core, @function
fp_nullmap_any_avx512_core:
	xorl	%eax, %eax
	testq	%rsi, %rsi
	jz	2f
1:	vmovdqu64	(%rdi,%rax), %zmm1
	vptestmq	%zmm1, %zmm1, %k1
	kortestq	%k1, %k1
	jnz	3f
	addq	$64, %rax
	cmpq	%rsi, %rax
	jb	1b
2:	xorl	%eax, %eax
	ret
3:	movl	$1, %eax
	ret
	.size	fp_nullmap_any_avx512_core, .-fp_nullmap_any_avx512_core

#endif

	.section	.note.GNU-stack,"",@progbits
