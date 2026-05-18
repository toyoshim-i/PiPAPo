/*
 * math.c — weak, arch-neutral 64-bit math helpers
 *
 * libgcc-style helpers (__udivdi3 / __ashldi3 / etc.) in portable C.
 * Symbols are weak so an arch that links a working toolchain libgcc
 * lets libgcc win automatically; an arch that does not link libgcc
 * (e.g. xtensa, whose libgcc is windowed-ABI and incompatible with
 * PPAP's call0 user-space) gets these definitions instead.
 *
 * Endianness-neutral: word splits are done with arithmetic shifts and
 * 32-bit casts, never via a `union { u64; u32[2]; }` pun.
 */

#include <stdint.h>

#define PPAP_WEAK __attribute__((weak))

typedef int64_t di_int;
typedef uint64_t du_int;
typedef int32_t si_int;
typedef uint32_t su_int;

PPAP_WEAK du_int __udivmoddi4(du_int n, du_int d, du_int *rp) {
  du_int q = 0;
  du_int r = 0;
  for (int i = 0; i < 64; ++i) {
    r = (r << 1) | (n >> 63);
    n <<= 1;
    if (r >= d) {
      r -= d;
      q = (q << 1) | 1u;
    } else {
      q <<= 1;
    }
  }
  if (rp) *rp = r;
  return q;
}

PPAP_WEAK du_int __udivdi3(du_int a, du_int b) {
  return __udivmoddi4(a, b, 0);
}

PPAP_WEAK du_int __umoddi3(du_int a, du_int b) {
  du_int r;
  __udivmoddi4(a, b, &r);
  return r;
}

PPAP_WEAK di_int __divdi3(di_int a, di_int b) {
  di_int sa = a >> 63;
  di_int sb = b >> 63;
  du_int ua = (du_int)((a ^ sa) - sa);
  du_int ub = (du_int)((b ^ sb) - sb);
  di_int s = sa ^ sb;
  di_int q = (di_int)__udivdi3(ua, ub);
  return (q ^ s) - s;
}

PPAP_WEAK di_int __moddi3(di_int a, di_int b) {
  di_int sa = a >> 63;
  di_int sb = b >> 63;
  du_int ua = (du_int)((a ^ sa) - sa);
  du_int ub = (du_int)((b ^ sb) - sb);
  du_int r;
  __udivmoddi4(ua, ub, &r);
  return ((di_int)r ^ sa) - sa;
}

/* Shift helpers split the 64-bit operand into 32-bit halves via
 * `(uint32_t)x` for the low word and `(uint32_t)(x >> 32)` for the
 * high word.  Constant-32 shifts are inlined by the compiler as a
 * word swap, so neither helper recurses into another 64-bit helper. */

PPAP_WEAK di_int __ashldi3(di_int a, int b) {
  if (b <= 0) return a;
  if (b >= 64) return 0;
  su_int lo = (su_int)(du_int)a;
  su_int hi = (su_int)((du_int)a >> 32);
  su_int out_lo, out_hi;
  if (b >= 32) {
    out_lo = 0;
    out_hi = lo << (b - 32);
  } else {
    out_lo = lo << b;
    out_hi = (hi << b) | (lo >> (32 - b));
  }
  return (di_int)(((du_int)out_hi << 32) | out_lo);
}

PPAP_WEAK di_int __ashrdi3(di_int a, int b) {
  if (b <= 0) return a;
  su_int lo = (su_int)(du_int)a;
  si_int hi = (si_int)((du_int)a >> 32);
  si_int sign = hi >> 31;
  su_int out_lo, out_hi;
  if (b >= 64) {
    out_lo = (su_int)sign;
    out_hi = (su_int)sign;
  } else if (b >= 32) {
    out_lo = (su_int)(hi >> (b - 32));
    out_hi = (su_int)sign;
  } else {
    out_lo = (lo >> b) | ((su_int)hi << (32 - b));
    out_hi = (su_int)(hi >> b);
  }
  return (di_int)(((du_int)out_hi << 32) | out_lo);
}

PPAP_WEAK du_int __lshrdi3(du_int a, int b) {
  if (b <= 0) return a;
  if (b >= 64) return 0;
  su_int lo = (su_int)a;
  su_int hi = (su_int)(a >> 32);
  su_int out_lo, out_hi;
  if (b >= 32) {
    out_lo = hi >> (b - 32);
    out_hi = 0;
  } else {
    out_lo = (lo >> b) | (hi << (32 - b));
    out_hi = hi >> b;
  }
  return ((du_int)out_hi << 32) | out_lo;
}

PPAP_WEAK du_int __muldi3(du_int a, du_int b) {
  su_int a_lo = (su_int)a;
  su_int a_hi = (su_int)(a >> 32);
  su_int b_lo = (su_int)b;
  su_int b_hi = (su_int)(b >> 32);
  /* Full 64-bit product of the low halves plus the truncated cross
   * terms (overflow above bit 63 is intentionally discarded, matching
   * libgcc's __muldi3). */
  du_int lo_lo = (du_int)a_lo * (du_int)b_lo;
  su_int cross = a_lo * b_hi + a_hi * b_lo;
  su_int out_lo = (su_int)lo_lo;
  su_int out_hi = (su_int)(lo_lo >> 32) + cross;
  return ((du_int)out_hi << 32) | out_lo;
}
