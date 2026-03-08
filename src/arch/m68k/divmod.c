/*
 * divmod.c — 32-bit division/modulo for Motorola 68000
 *
 * The system libgcc (__udivsi3, __umodsi3) uses BSR.L (a 68020+
 * instruction) which faults on the 68000.  These replacements use
 * only shift-and-subtract, generating pure 68000 code.
 *
 * GCC emits calls to these when compiling with -m68000 and the
 * source uses / or % operators on 32-bit values.
 */

#include <stdint.h>

/* Shift-and-subtract unsigned 32÷32 division.
 * Returns quotient; stores remainder in *rem if non-NULL. */
static uint32_t udivmod32(uint32_t num, uint32_t den, uint32_t *rem)
{
    if (den == 0) {
        if (rem) *rem = 0;
        return 0;
    }

    uint32_t quot = 0;
    uint32_t r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((num >> i) & 1u);
        if (r >= den) {
            r -= den;
            quot |= (1u << i);
        }
    }
    if (rem) *rem = r;
    return quot;
}

/* GCC libgcc entry points */

uint32_t __udivsi3(uint32_t a, uint32_t b)
{
    return udivmod32(a, b, (void *)0);
}

uint32_t __umodsi3(uint32_t a, uint32_t b)
{
    uint32_t rem;
    udivmod32(a, b, &rem);
    return rem;
}

int32_t __divsi3(int32_t a, int32_t b)
{
    int neg = 0;
    uint32_t ua = (uint32_t)a;
    uint32_t ub = (uint32_t)b;
    if (a < 0) { ua = (uint32_t)(-a); neg = !neg; }
    if (b < 0) { ub = (uint32_t)(-b); neg = !neg; }
    uint32_t q = udivmod32(ua, ub, (void *)0);
    return neg ? -(int32_t)q : (int32_t)q;
}

int32_t __modsi3(int32_t a, int32_t b)
{
    int neg = (a < 0);
    uint32_t ua = neg ? (uint32_t)(-a) : (uint32_t)a;
    uint32_t ub = (b < 0) ? (uint32_t)(-b) : (uint32_t)b;
    uint32_t rem;
    udivmod32(ua, ub, &rem);
    return neg ? -(int32_t)rem : (int32_t)rem;
}
