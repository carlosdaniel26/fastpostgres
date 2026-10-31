/* kernels_avx2.S — hand-written AVX2 kernels for the marshalling hot path.
 *
 * System V AMD64 ABI, position independent throughout (register operands only,
 * so no relocation can turn into a TEXTREL — Postgres refuses to load a module
 * that has one). Integer and 128-bit moves only: no upper vector state is left
 * dirty, so no vzeroupper is needed on these paths.
 *
 * Each *_core function handles whole blocks only. The block size is a contract
 * with the C wrapper in kernels.c, which runs the tail — that keeps the asm
 * free of partial-block special cases, which is where hand-written SIMD
 * usually goes wrong.
 */
#if defined(__x86_64__)

	.text

/* int fp_nullmap_any_avx2_core(const uint8_t *bits, size_t nbytes)
 *   rdi = bits, rsi = nbytes (a multiple of 32; 0 is a multiple of 32).
 *   Returns 1 if any byte is set. vptest sets ZF when (a & b) is zero, so the
 *   data is tested against itself — testing against a zero register would
 *   always say "nothing" (that bug is why kernels_bench.c exists). */
	.globl	fp_nullmap_any_avx2_core
	.type	fp_nullmap_any_avx2_core, @function
fp_nullmap_any_avx2_core:
	xorl	%eax, %eax
	testq	%rsi, %rsi
	jz	2f
1:	vmovdqu	(%rdi,%rax), %ymm1
	vptest	%ymm1, %ymm1
	jnz	3f
	addq	$32, %rax
	cmpq	%rsi, %rax
	jb	1b
2:	xorl	%eax, %eax
	ret
3:	movl	$1, %eax
	ret
	.size	fp_nullmap_any_avx2_core, .-fp_nullmap_any_avx2_core

/* void fp_copy_small_avx2(void *dst, const void *src, size_t len)
 *   rdi = dst, rsi = src, rdx = len (len <= 32).
 *   The classic size ladder: two overlapping moves per class, taken from the
 *   head and the tail, so nothing outside [0, len) is read or written. That is
 *   what lets the encoder call this straight into the segment with no slack,
 *   and what makes len == 0 a fall-through that must return early — a zero-
 *   length copy that wrote a byte would fault against a page boundary. */
	.globl	fp_copy_small_avx2
	.type	fp_copy_small_avx2, @function
fp_copy_small_avx2:
	testq	%rdx, %rdx
	jz	.Ldone
	cmpq	$16, %rdx
	jae	.Lc16
	cmpq	$8, %rdx
	jae	.Lc8
	cmpq	$4, %rdx
	jae	.Lc4
	cmpq	$2, %rdx
	jae	.Lc2
	movzbl	(%rsi), %eax
	movb	%al, (%rdi)
.Ldone:
	ret
.Lc2:
	movzwl	(%rsi), %eax
	movzwl	-2(%rsi,%rdx), %ecx
	movw	%ax, (%rdi)
	movw	%cx, -2(%rdi,%rdx)
	ret
.Lc4:
	movl	(%rsi), %eax
	movl	-4(%rsi,%rdx), %ecx
	movl	%eax, (%rdi)
	movl	%ecx, -4(%rdi,%rdx)
	ret
.Lc8:
	movq	(%rsi), %rax
	movq	-8(%rsi,%rdx), %rcx
	movq	%rax, (%rdi)
	movq	%rcx, -8(%rdi,%rdx)
	ret
.Lc16:
	vmovdqu	(%rsi), %xmm0
	vmovdqu	-16(%rsi,%rdx), %xmm1
	vmovdqu	%xmm0, (%rdi)
	vmovdqu	%xmm1, -16(%rdi,%rdx)
	ret
	.size	fp_copy_small_avx2, .-fp_copy_small_avx2

#endif

	.section	.note.GNU-stack,"",@progbits
