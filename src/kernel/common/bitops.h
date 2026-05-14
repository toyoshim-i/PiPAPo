/*
 * bitops.h — Bit-twiddling helpers (count-trailing-zeros, etc.)
 *
 * Hand-rolled implementations.  Avoid `__builtin_ctz` / `__builtin_clz` /
 * `__builtin_popcount` because they lower to libgcc runtime calls
 * (__ctzsi2 / __clzsi2 / __popcountsi2) on every PPAP target that
 * doesn't have a single-instruction equivalent — Cortex-M0+, m68k,
 * RV32 without Zbb, ia16.  Distro libgcc cross builds use flags PPAP
 * cannot rely on (no-PIC, wrong CPU baseline, wrong multilib).
 *
 * On targets that do have native CTZ/CLZ (ARM v7-M+, RV32 with Zbb)
 * the compiler still recognises the bit-search idiom in `ctz32` and
 * folds it to the same single-instruction sequence as the builtin.
 */

#ifndef PPAP_KERNEL_COMMON_BITOPS_H
#define PPAP_KERNEL_COMMON_BITOPS_H

#include <stdint.h>

/* Count trailing zeros of a uint32_t.  Undefined when x == 0. */
static inline int ctz32(uint32_t x) {
  int n = 0;
  if (!(x & 0xFFFF)) {
    n += 16;
    x >>= 16;
  }
  if (!(x & 0xFF)) {
    n += 8;
    x >>= 8;
  }
  if (!(x & 0xF)) {
    n += 4;
    x >>= 4;
  }
  if (!(x & 0x3)) {
    n += 2;
    x >>= 2;
  }
  if (!(x & 0x1)) {
    n += 1;
  }
  return n;
}

#endif /* PPAP_KERNEL_COMMON_BITOPS_H */
