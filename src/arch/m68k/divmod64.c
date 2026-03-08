/*
 * divmod64.c — 64-bit arithmetic helpers for Motorola 68000
 *
 * The system libgcc (__muldi3, __divdi3, etc.) uses 68020+ instructions
 * (MULS.L, DIVU.L, EXTB.L, BSR.L) which fault on the 68000.  These
 * replacements use only 32-bit and 16-bit operations.
 *
 * CRITICAL: This code must NOT use C's int64_t multiply, divide, or
 * modulo operators, as GCC will emit recursive calls to the very
 * functions we're replacing → infinite loop or stack overflow.
 * All 64-bit arithmetic is done manually with 32-bit ops.
 */

#include <stdint.h>

/* ── 32×32 → 64 unsigned multiply ──────────────────────────────────── */

typedef union {
    uint64_t u64;
    struct {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        uint32_t hi, lo;
#else
        uint32_t lo, hi;
#endif
    } w;
} u64_parts;

static void umul32x32(uint32_t a, uint32_t b, uint32_t *hi, uint32_t *lo)
{
    uint32_t a_lo = a & 0xffff;
    uint32_t a_hi = a >> 16;
    uint32_t b_lo = b & 0xffff;
    uint32_t b_hi = b >> 16;

    uint32_t p0 = a_lo * b_lo;
    uint32_t p1 = a_lo * b_hi;
    uint32_t p2 = a_hi * b_lo;
    uint32_t p3 = a_hi * b_hi;

    uint32_t mid = p1 + p2;
    uint32_t carry_mid = (mid < p1) ? 1u : 0u;

    uint32_t lo_result = p0 + (mid << 16);
    uint32_t carry_lo = (lo_result < p0) ? 1u : 0u;

    *lo = lo_result;
    *hi = p3 + (mid >> 16) + (carry_mid << 16) + carry_lo;
}

/* ── 64÷64 → 64 unsigned divide (shift-subtract) ──────────────────── */

static void udivmod64(uint32_t n_hi, uint32_t n_lo,
                      uint32_t d_hi, uint32_t d_lo,
                      uint32_t *q_hi, uint32_t *q_lo,
                      uint32_t *r_hi, uint32_t *r_lo)
{
    /* Division by zero → return 0 */
    if (d_hi == 0 && d_lo == 0) {
        *q_hi = *q_lo = *r_hi = *r_lo = 0;
        return;
    }

    /* Remainder accumulator */
    uint32_t rem_hi = 0, rem_lo = 0;
    uint32_t quot_hi = 0, quot_lo = 0;

    for (int i = 63; i >= 0; i--) {
        /* rem = (rem << 1) | bit i of numerator */
        rem_hi = (rem_hi << 1) | (rem_lo >> 31);
        rem_lo <<= 1;
        if (i >= 32)
            rem_lo |= (n_hi >> (i - 32)) & 1u;
        else
            rem_lo |= (n_lo >> i) & 1u;

        /* if rem >= divisor: rem -= divisor, set quotient bit */
        int ge = 0;
        if (rem_hi > d_hi) ge = 1;
        else if (rem_hi == d_hi && rem_lo >= d_lo) ge = 1;

        if (ge) {
            /* rem -= divisor (64-bit subtract) */
            uint32_t borrow = (rem_lo < d_lo) ? 1u : 0u;
            rem_lo -= d_lo;
            rem_hi -= d_hi + borrow;

            if (i >= 32)
                quot_hi |= (1u << (i - 32));
            else
                quot_lo |= (1u << i);
        }
    }

    *q_hi = quot_hi;
    *q_lo = quot_lo;
    *r_hi = rem_hi;
    *r_lo = rem_lo;
}

/* ── Negate a 64-bit value (two's complement) ──────────────────────── */

static void neg64(uint32_t *hi, uint32_t *lo)
{
    *lo = ~(*lo) + 1;
    *hi = ~(*hi) + (*lo == 0 ? 1u : 0u);
}

/* ── GCC libgcc entry points ───────────────────────────────────────── */

uint64_t __muldi3(uint64_t a, uint64_t b)
{
    u64_parts pa, pb, pr;
    pa.u64 = a;
    pb.u64 = b;

    /* (a_hi*2^32 + a_lo) * (b_hi*2^32 + b_lo)
     * = a_lo*b_lo + (a_lo*b_hi + a_hi*b_lo)*2^32
     * (high 64 bits of a_hi*b_hi*2^64 overflow and are discarded) */
    uint32_t res_hi, res_lo;
    umul32x32(pa.w.lo, pb.w.lo, &res_hi, &res_lo);

    /* Add cross terms (only low 32 bits matter for result_hi) */
    uint32_t cross1_hi, cross1_lo;
    umul32x32(pa.w.lo, pb.w.hi, &cross1_hi, &cross1_lo);
    res_hi += cross1_lo;

    uint32_t cross2_hi, cross2_lo;
    umul32x32(pa.w.hi, pb.w.lo, &cross2_hi, &cross2_lo);
    res_hi += cross2_lo;

    pr.w.lo = res_lo;
    pr.w.hi = res_hi;
    return pr.u64;
}

uint64_t __udivdi3(uint64_t a, uint64_t b)
{
    u64_parts pa, pb, pq;
    pa.u64 = a;
    pb.u64 = b;
    uint32_t r_hi, r_lo;
    udivmod64(pa.w.hi, pa.w.lo, pb.w.hi, pb.w.lo,
              &pq.w.hi, &pq.w.lo, &r_hi, &r_lo);
    return pq.u64;
}

uint64_t __umoddi3(uint64_t a, uint64_t b)
{
    u64_parts pa, pb, pr;
    pa.u64 = a;
    pb.u64 = b;
    uint32_t q_hi, q_lo;
    udivmod64(pa.w.hi, pa.w.lo, pb.w.hi, pb.w.lo,
              &q_hi, &q_lo, &pr.w.hi, &pr.w.lo);
    return pr.u64;
}

int64_t __divdi3(int64_t a, int64_t b)
{
    u64_parts pa, pb, pq;
    int negate = 0;

    pa.u64 = (uint64_t)a;
    pb.u64 = (uint64_t)b;

    if (a < 0) { neg64(&pa.w.hi, &pa.w.lo); negate = 1; }
    if (b < 0) { neg64(&pb.w.hi, &pb.w.lo); negate ^= 1; }

    uint32_t r_hi, r_lo;
    udivmod64(pa.w.hi, pa.w.lo, pb.w.hi, pb.w.lo,
              &pq.w.hi, &pq.w.lo, &r_hi, &r_lo);

    if (negate) neg64(&pq.w.hi, &pq.w.lo);
    return (int64_t)pq.u64;
}

int64_t __moddi3(int64_t a, int64_t b)
{
    u64_parts pa, pb, pr;
    int negate = 0;

    pa.u64 = (uint64_t)a;
    pb.u64 = (uint64_t)b;

    if (a < 0) { neg64(&pa.w.hi, &pa.w.lo); negate = 1; }
    if (b < 0) { neg64(&pb.w.hi, &pb.w.lo); }

    uint32_t q_hi, q_lo;
    udivmod64(pa.w.hi, pa.w.lo, pb.w.hi, pb.w.lo,
              &q_hi, &q_lo, &pr.w.hi, &pr.w.lo);

    if (negate) neg64(&pr.w.hi, &pr.w.lo);
    return (int64_t)pr.u64;
}
