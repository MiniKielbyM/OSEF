/* OSEF bare-metal: minimal setjmp/longjmp (System V x86_64).
 *
 * osef_jmp_buf layout (8 x uint64_t): rbx, rbp, r12, r13, r14, r15, rsp, rip
 *
 *   int  osef_setjmp(osef_jmp_buf env);              // 0 on the direct call
 *   void osef_longjmp(osef_jmp_buf env, int value);  // setjmp returns value (1 if 0)
 *
 * SSE/x87 state is not saved: under the SysV ABI all xmm registers are
 * caller-saved.
 */

    .text
    .code64

    .globl osef_setjmp
    .type osef_setjmp, @function
    .p2align 4
osef_setjmp:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rdx          /* caller's rsp after our return */
    movq %rdx, 48(%rdi)
    movq (%rsp), %rdx           /* return address */
    movq %rdx, 56(%rdi)
    xorl %eax, %eax
    ret
    .size osef_setjmp, . - osef_setjmp

    .globl osef_longjmp
    .type osef_longjmp, @function
    .p2align 4
osef_longjmp:
    movl %esi, %eax
    testl %eax, %eax
    jnz 1f
    movl $1, %eax
1:
    movq 0(%rdi), %rbx
    movq 8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    jmpq *56(%rdi)
    .size osef_longjmp, . - osef_longjmp

    .section .note.GNU-stack,"",@progbits
