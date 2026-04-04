/* math.S — 68000-safe integer math helpers for the kernel
 *
 * Provides __mulsi3, __udivsi3, __umodsi3 so the kernel does not need
 * to link libgcc (which is built with -msep-data -fPIC and requires
 * a5 as GOT base — not available in kernel context).
 */

    .text

/* --- __mulsi3: 32-bit multiply ---
 * unsigned long __mulsi3(unsigned long a, unsigned long b);
 *
 * 68000 has only 16×16→32 mulu.  Split into partial products:
 *   a = (ah << 16) | al,  b = (bh << 16) | bl
 *   a*b = (ah*bl + al*bh) << 16 + al*bl
 */
    .globl  __mulsi3
    .type   __mulsi3, @function
__mulsi3:
    move.w  4+2(%sp), %d0       /* d0 = al (low 16 bits of a) */
    mulu    8+2(%sp), %d0       /* d0 = al * bl (32-bit result) */
    move.w  8+2(%sp), %d1       /* d1 = bl */
    mulu    4(%sp), %d1         /* d1 = ah * bl */
    swap    %d1
    clr.w   %d1                 /* d1 = (ah * bl) << 16 */
    add.l   %d1, %d0
    move.w  4+2(%sp), %d1       /* d1 = al */
    mulu    8(%sp), %d1         /* d1 = al * bh */
    swap    %d1
    clr.w   %d1                 /* d1 = (al * bh) << 16 */
    add.l   %d1, %d0
    rts
    .size   __mulsi3, . - __mulsi3

/* --- __udivsi3: unsigned 32-bit divide ---
 * unsigned long __udivsi3(unsigned long n, unsigned long d);
 *
 * 68000 has only 32/16→16 divu which traps on overflow.
 * Use shift-and-subtract long division for the general case.
 */
    .globl  __udivsi3
    .type   __udivsi3, @function
__udivsi3:
    move.l  4(%sp), %d0         /* d0 = numerator */
    move.l  8(%sp), %d1         /* d1 = divisor */
    /* Fall through to internal routine; quotient returned in d0 */
    bsr.w   __udivsi3_internal
    rts
    .size   __udivsi3, . - __udivsi3

/* --- __umodsi3: unsigned 32-bit modulo ---
 * unsigned long __umodsi3(unsigned long n, unsigned long d);
 */
    .globl  __umodsi3
    .type   __umodsi3, @function
__umodsi3:
    move.l  4(%sp), %d0         /* d0 = numerator */
    move.l  8(%sp), %d1         /* d1 = divisor */
    bsr.w   __udivsi3_internal  /* d0 = quotient, d1 = remainder */
    move.l  %d1, %d0            /* return remainder */
    rts
    .size   __umodsi3, . - __umodsi3

/* --- internal: 32-bit unsigned division via shift-and-subtract ---
 * Input:  d0 = numerator, d1 = divisor
 * Output: d0 = quotient,  d1 = remainder
 * Clobbers: d2, d3
 */
    .type   __udivsi3_internal, @function
__udivsi3_internal:
    movem.l %d2-%d3, -(%sp)
    tst.l   %d1
    beq.w   .Ldiv_zero          /* division by zero → return 0 */

    /* Shift-and-subtract long division: 32 iterations */
    clr.l   %d2                 /* d2 = remainder = 0 */
    moveq   #31, %d3            /* d3 = bit counter */
.Lloop:
    add.l   %d0, %d0            /* shift numerator left, MSB → X flag */
    addx.l  %d2, %d2            /* shift remainder left, pull in carry */
    cmp.l   %d1, %d2
    bcs.w   .Lno_sub            /* remainder < divisor, skip */
    sub.l   %d1, %d2            /* remainder -= divisor */
    addq.l  #1, %d0             /* set quotient bit */
.Lno_sub:
    dbra    %d3, .Lloop

    move.l  %d2, %d1            /* d1 = remainder */
    movem.l (%sp)+, %d2-%d3
    rts

.Ldiv_zero:
    clr.l   %d0
    clr.l   %d1
    movem.l (%sp)+, %d2-%d3
    rts
    .size   __udivsi3_internal, . - __udivsi3_internal
