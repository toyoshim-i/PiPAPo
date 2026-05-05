@ setjmp.S — Cortex-M setjmp / longjmp (Thumb-1 compatible)
@
@ Layout in jmp_buf:
@   0   r4
@   4   r5
@   8   r6
@  12   r7
@  16   r8
@  20   r9
@  24   r10
@  28   r11
@  32   sp
@  36   lr
@
@ Thumb-1 (Cortex-M0+) cannot stmia high registers, so we move via a
@ low scratch register one at a time.  Cortex-M3/4 supports the wider
@ encoding but the simple version assembles for both.

.syntax unified
.thumb

.section .text.setjmp, "ax"
.global setjmp
.thumb_func
setjmp:
    str     r4, [r0, #0]
    str     r5, [r0, #4]
    str     r6, [r0, #8]
    str     r7, [r0, #12]
    mov     r1, r8
    str     r1, [r0, #16]
    mov     r1, r9
    str     r1, [r0, #20]
    mov     r1, r10
    str     r1, [r0, #24]
    mov     r1, r11
    str     r1, [r0, #28]
    mov     r1, sp
    str     r1, [r0, #32]
    mov     r1, lr
    str     r1, [r0, #36]
    movs    r0, #0
    bx      lr

.section .text.longjmp, "ax"
.global longjmp
.thumb_func
longjmp:
    @ r0 = env, r1 = val
    ldr     r4, [r0, #0]
    ldr     r5, [r0, #4]
    ldr     r6, [r0, #8]
    ldr     r7, [r0, #12]
    ldr     r2, [r0, #16]
    mov     r8, r2
    ldr     r2, [r0, #20]
    mov     r9, r2
    ldr     r2, [r0, #24]
    mov     r10, r2
    ldr     r2, [r0, #28]
    mov     r11, r2
    ldr     r2, [r0, #32]
    mov     sp, r2
    ldr     r3, [r0, #36]
    @ POSIX: longjmp(env, 0) appears as setjmp returning 1.
    cmp     r1, #0
    bne     1f
    movs    r1, #1
1:  mov     r0, r1
    bx      r3
