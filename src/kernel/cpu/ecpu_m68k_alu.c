/*
 * ecpu_m68k_alu.c — m68k ALU operations and CCR computation
 *
 * Step 2: ADD/SUB/CMP/NEG/NEGX/TST/EXT/MULU/MULS/DIVU/DIVS
 *         with full CCR (X/N/Z/V/C) computation.
 *
 * See docs/ecpu/m68k.md §8 for flag computation rules.
 */

#include "ecpu_m68k.h"

/* ── ADD with CCR ───────────────────────────────────────────────────────── */

uint32_t m68k_alu_add(m68k_state_t *cpu, uint32_t src, uint32_t dst,
                      uint8_t size) {
  uint32_t mask = m68k_size_mask(size);
  uint32_t msb = m68k_size_msb(size);
  uint64_t res = (uint64_t)(dst & mask) + (uint64_t)(src & mask);
  uint32_t result = (uint32_t)res & mask;

  uint16_t ccr = 0;
  if (res & ((uint64_t)mask + 1)) ccr |= M68K_FLAG_C | M68K_FLAG_X;
  if (result == 0) ccr |= M68K_FLAG_Z;
  if (result & msb) ccr |= M68K_FLAG_N;
  if ((~(src ^ dst) & (src ^ result)) & msb) ccr |= M68K_FLAG_V;
  cpu->sr = (cpu->sr & 0xFF00) | ccr;

  return result;
}

/* ── SUB with CCR ───────────────────────────────────────────────────────── */

uint32_t m68k_alu_sub(m68k_state_t *cpu, uint32_t src, uint32_t dst,
                      uint8_t size) {
  uint32_t mask = m68k_size_mask(size);
  uint32_t msb = m68k_size_msb(size);
  uint32_t s = src & mask;
  uint32_t d = dst & mask;
  uint32_t result = (d - s) & mask;

  uint16_t ccr = 0;
  if (s > d) ccr |= M68K_FLAG_C | M68K_FLAG_X;
  if (result == 0) ccr |= M68K_FLAG_Z;
  if (result & msb) ccr |= M68K_FLAG_N;
  if (((src ^ dst) & (dst ^ result)) & msb) ccr |= M68K_FLAG_V;
  cpu->sr = (cpu->sr & 0xFF00) | ccr;

  return result;
}

/* ── CMP (like SUB but X not affected) ──────────────────────────────────── */

void m68k_alu_cmp(m68k_state_t *cpu, uint32_t src, uint32_t dst, uint8_t size) {
  uint32_t mask = m68k_size_mask(size);
  uint32_t msb = m68k_size_msb(size);
  uint32_t s = src & mask;
  uint32_t d = dst & mask;
  uint32_t result = (d - s) & mask;

  /* Preserve X flag */
  uint16_t ccr = cpu->sr & M68K_FLAG_X;
  if (s > d) ccr |= M68K_FLAG_C;
  if (result == 0) ccr |= M68K_FLAG_Z;
  if (result & msb) ccr |= M68K_FLAG_N;
  if (((src ^ dst) & (dst ^ result)) & msb) ccr |= M68K_FLAG_V;
  cpu->sr = (cpu->sr & 0xFF00) | ccr;
}

/* ── NEG (0 - dst) ──────────────────────────────────────────────────────── */

uint32_t m68k_alu_neg(m68k_state_t *cpu, uint32_t dst, uint8_t size) {
  return m68k_alu_sub(cpu, dst, 0, size);
}

/* ── NEGX (0 - dst - X) ────────────────────────────────────────────────── */

uint32_t m68k_alu_negx(m68k_state_t *cpu, uint32_t dst, uint8_t size) {
  uint32_t mask = m68k_size_mask(size);
  uint32_t msb = m68k_size_msb(size);
  uint32_t x = (cpu->sr & M68K_FLAG_X) ? 1 : 0;
  uint32_t d = dst & mask;
  uint64_t res = (uint64_t)0 - (uint64_t)d - (uint64_t)x;
  uint32_t result = (uint32_t)res & mask;

  uint16_t ccr = 0;
  if (d != 0 || x != 0) ccr |= M68K_FLAG_C | M68K_FLAG_X;
  /* Z: cleared if result non-zero, unchanged if zero */
  if (result != 0)
    ccr &= ~M68K_FLAG_Z; /* clear Z */
  else
    ccr |= (cpu->sr & M68K_FLAG_Z); /* preserve Z */
  if (result & msb) ccr |= M68K_FLAG_N;
  if ((dst & result) & msb) ccr |= M68K_FLAG_V;
  cpu->sr = (cpu->sr & 0xFF00) | ccr;

  return result;
}

/* ── TST (set CCR from value, X unchanged) ──────────────────────────────── */

void m68k_alu_tst(m68k_state_t *cpu, uint32_t val, uint8_t size) {
  uint32_t mask = m68k_size_mask(size);
  uint32_t msb = m68k_size_msb(size);

  uint16_t ccr = cpu->sr & M68K_FLAG_X;
  if ((val & mask) == 0) ccr |= M68K_FLAG_Z;
  if (val & msb) ccr |= M68K_FLAG_N;
  /* V and C cleared */
  cpu->sr = (cpu->sr & 0xFF00) | ccr;
}

/* ── MULU (unsigned 16×16→32) ───────────────────────────────────────────── */

uint32_t m68k_alu_mulu(m68k_state_t *cpu, uint16_t src, uint16_t dst) {
  uint32_t result = (uint32_t)src * (uint32_t)dst;

  uint16_t ccr = cpu->sr & M68K_FLAG_X;
  if (result == 0) ccr |= M68K_FLAG_Z;
  if (result & 0x80000000) ccr |= M68K_FLAG_N;
  /* V=0, C=0 */
  cpu->sr = (cpu->sr & 0xFF00) | ccr;

  return result;
}

/* ── MULS (signed 16×16→32) ─────────────────────────────────────────────── */

uint32_t m68k_alu_muls(m68k_state_t *cpu, int16_t src, int16_t dst) {
  int32_t result = (int32_t)src * (int32_t)dst;
  uint32_t uresult = (uint32_t)result;

  uint16_t ccr = cpu->sr & M68K_FLAG_X;
  if (uresult == 0) ccr |= M68K_FLAG_Z;
  if (uresult & 0x80000000) ccr |= M68K_FLAG_N;
  /* V=0, C=0 */
  cpu->sr = (cpu->sr & 0xFF00) | ccr;

  return uresult;
}

/* ── ADDX (dst + src + X, sticky Z) ────────────────────────────────────── */

uint32_t m68k_alu_addx(m68k_state_t *cpu, uint32_t src, uint32_t dst,
                       uint8_t size) {
  uint32_t mask = m68k_size_mask(size);
  uint32_t msb = m68k_size_msb(size);
  uint32_t x = (cpu->sr & M68K_FLAG_X) ? 1 : 0;
  uint64_t res = (uint64_t)(dst & mask) + (uint64_t)(src & mask) + x;
  uint32_t result = (uint32_t)res & mask;

  uint16_t ccr = 0;
  if (res & ((uint64_t)mask + 1)) ccr |= M68K_FLAG_C | M68K_FLAG_X;
  /* Sticky Z: cleared if non-zero, preserved if zero */
  if (result != 0)
    ccr &= ~M68K_FLAG_Z;
  else
    ccr |= (cpu->sr & M68K_FLAG_Z);
  if (result & msb) ccr |= M68K_FLAG_N;
  if ((~(src ^ dst) & (src ^ result)) & msb) ccr |= M68K_FLAG_V;
  cpu->sr = (cpu->sr & 0xFF00) | ccr;

  return result;
}

/* ── SUBX (dst - src - X, sticky Z) ───────────────────────────────────── */

uint32_t m68k_alu_subx(m68k_state_t *cpu, uint32_t src, uint32_t dst,
                       uint8_t size) {
  uint32_t mask = m68k_size_mask(size);
  uint32_t msb = m68k_size_msb(size);
  uint32_t x = (cpu->sr & M68K_FLAG_X) ? 1 : 0;
  uint32_t s = src & mask;
  uint32_t d = dst & mask;
  uint64_t borrow = (uint64_t)s + x;
  uint32_t result = (d - s - x) & mask;

  uint16_t ccr = 0;
  if (borrow > d) ccr |= M68K_FLAG_C | M68K_FLAG_X;
  /* Sticky Z */
  if (result != 0)
    ccr &= ~M68K_FLAG_Z;
  else
    ccr |= (cpu->sr & M68K_FLAG_Z);
  if (result & msb) ccr |= M68K_FLAG_N;
  if (((src ^ dst) & (dst ^ result)) & msb) ccr |= M68K_FLAG_V;
  cpu->sr = (cpu->sr & 0xFF00) | ccr;

  return result;
}

/* ── ABCD (BCD add with extend) ───────────────────────────────────────── */

uint32_t m68k_alu_abcd(m68k_state_t *cpu, uint8_t src, uint8_t dst) {
  uint32_t x = (cpu->sr & M68K_FLAG_X) ? 1 : 0;
  uint32_t lo = (dst & 0x0F) + (src & 0x0F) + x;
  uint32_t hi = (dst & 0xF0) + (src & 0xF0);
  uint32_t carry = 0;

  if (lo > 9) {
    lo += 6;
  }
  hi += (lo & 0xF0); /* carry from low nybble */
  if (hi > 0x90) {
    hi += 0x60;
    carry = 1;
  }
  uint8_t result = (hi & 0xF0) | (lo & 0x0F);

  uint16_t ccr = 0;
  if (carry) ccr |= M68K_FLAG_C | M68K_FLAG_X;
  /* Z: cleared if non-zero, unchanged otherwise (sticky) */
  if (result != 0)
    ccr &= ~M68K_FLAG_Z;
  else
    ccr |= (cpu->sr & M68K_FLAG_Z);
  cpu->sr = (cpu->sr & 0xFF00) | ccr;

  return result;
}

/* ── SBCD (BCD subtract with extend) ──────────────────────────────────── */

uint32_t m68k_alu_sbcd(m68k_state_t *cpu, uint8_t src, uint8_t dst) {
  uint32_t x = (cpu->sr & M68K_FLAG_X) ? 1 : 0;
  int lo = (dst & 0x0F) - (src & 0x0F) - x;
  int hi = (dst & 0xF0) - (src & 0xF0);
  uint32_t carry = 0;

  if (lo < 0) {
    lo += 10;
    hi -= 0x10;
  }
  if (hi < 0) {
    hi += 0xA0;
    carry = 1;
  }
  uint8_t result = (hi & 0xF0) | (lo & 0x0F);

  uint16_t ccr = 0;
  if (carry) ccr |= M68K_FLAG_C | M68K_FLAG_X;
  /* Sticky Z */
  if (result != 0)
    ccr &= ~M68K_FLAG_Z;
  else
    ccr |= (cpu->sr & M68K_FLAG_Z);
  cpu->sr = (cpu->sr & 0xFF00) | ccr;

  return result;
}

/* ── NBCD (BCD negate with extend: 0 - dst - X) ──────────────────────── */

uint32_t m68k_alu_nbcd(m68k_state_t *cpu, uint8_t dst) {
  return m68k_alu_sbcd(cpu, dst, 0);
}

/* ── DIVU (unsigned 32÷16→16q:16r) ─────────────────────────────────────── */

int m68k_alu_divu(m68k_state_t *cpu, uint32_t dst, uint16_t src,
                  uint32_t *result) {
  if (src == 0) return -1; /* divide by zero — caller fires trap */

  uint32_t quotient = dst / src;
  uint32_t remainder = dst % src;

  if (quotient > 0xFFFF) {
    /* Overflow — V set, result not stored */
    cpu->sr = (cpu->sr & (0xFF00 | M68K_FLAG_X)) | M68K_FLAG_V;
    /* N flag is set based on MSB of quotient (68000 behavior) */
    if (quotient & 0x8000) cpu->sr |= M68K_FLAG_N;
    return 1; /* overflow (distinct from 0=ok, -1=divzero) */
  }

  *result = (remainder << 16) | (quotient & 0xFFFF);

  uint16_t ccr = cpu->sr & M68K_FLAG_X;
  if (quotient == 0) ccr |= M68K_FLAG_Z;
  if (quotient & 0x8000) ccr |= M68K_FLAG_N;
  /* V=0, C=0 */
  cpu->sr = (cpu->sr & 0xFF00) | ccr;

  return 0;
}

/* ── DIVS (signed 32÷16→16q:16r) ───────────────────────────────────────── */

int m68k_alu_divs(m68k_state_t *cpu, int32_t dst, int16_t src,
                  uint32_t *result) {
  if (src == 0) return -1; /* divide by zero */

  int32_t quotient = dst / src;
  int32_t remainder = dst % src;

  if (quotient > 32767 || quotient < -32768) {
    /* Overflow */
    cpu->sr = (cpu->sr & (0xFF00 | M68K_FLAG_X)) | M68K_FLAG_V;
    if (quotient & 0x8000) cpu->sr |= M68K_FLAG_N;
    return 1; /* overflow */
  }

  *result =
      ((uint32_t)(uint16_t)remainder << 16) | ((uint32_t)(uint16_t)quotient);

  uint16_t ccr = cpu->sr & M68K_FLAG_X;
  if ((quotient & 0xFFFF) == 0) ccr |= M68K_FLAG_Z;
  if (quotient & 0x8000) ccr |= M68K_FLAG_N;
  /* V=0, C=0 */
  cpu->sr = (cpu->sr & 0xFF00) | ccr;

  return 0;
}
