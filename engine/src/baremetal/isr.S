/* OSEF bare-metal: interrupt entry stubs for vectors 0-47.
 *
 * Every stub leaves the same frame on the stack (see InterruptFrame in
 * internal.h): CPU frame, error code (0 if the CPU pushed none), vector, then
 * rax..r15. The common path saves the SSE/x87 state with fxsave into a
 * 16-byte aligned area, calls osef_bm_isr_dispatch(InterruptFrame*), restores
 * everything and returns with iretq.
 */

    .text
    .code64

/* Vectors where the CPU pushes an error code: 8, 10-14, 17, 21, 29, 30. */
.macro ISR_NOERR vec
    .p2align 4
osef_bm_isr_\vec:
    pushq $0
    pushq $\vec
    jmp osef_bm_isr_common
.endm

.macro ISR_ERR vec
    .p2align 4
osef_bm_isr_\vec:
    pushq $\vec
    jmp osef_bm_isr_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

    .p2align 4
osef_bm_isr_common:
    cld
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rbx            /* rbx = InterruptFrame* (callee-saved) */
    subq $512, %rsp
    andq $-16, %rsp            /* 16-byte aligned fxsave area; also ABI call alignment */
    fxsave (%rsp)
    movq %rbx, %rdi
    call osef_bm_isr_dispatch
    fxrstor (%rsp)
    movq %rbx, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp             /* vector + error code */
    iretq

/* Vectors 48-255 are never programmed (no APIC, no software interrupts), but
 * a stray one -- e.g. a local-APIC spurious interrupt, whose vector the
 * firmware chose (often 0xFF) -- must not hit a not-present gate (#GP ->
 * panic). Spurious interrupts need no EOI, so simply return. */
    .p2align 4
    .globl osef_bm_isr_ignore
osef_bm_isr_ignore:
    iretq

/* Table of stub addresses, indexed by vector (used by idt.cpp). */
    .section .rodata
    .p2align 3
    .globl osef_bm_isr_stubs
osef_bm_isr_stubs:
.irp vec, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    .quad osef_bm_isr_\vec
.endr

    .section .note.GNU-stack,"",@progbits
