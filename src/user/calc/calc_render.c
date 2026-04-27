/*
 * calc_render.c — value -> digit string with grouping
 *
 * Pure: no syscalls, no IO.  Exercised by tests/host/test_calc_render.c.
 */

#include "calc.h"

/* Forward decl from calc_state.c — same translation unit logically, but
 * we keep it private to avoid leaking the helper into calc.h. */
extern uint64_t calc_mask(int64_t v, calc_width_t width);

static const char HEX_UPPER[] = "0123456789ABCDEF";

static int base_radix(calc_base_t base) {
  switch (base) {
  case CALC_BASE_BIN: return 2;
  case CALC_BASE_OCT: return 8;
  case CALC_BASE_DEC: return 10;
  case CALC_BASE_HEX: return 16;
  }
  return 10;
}

void calc_render_value(int64_t value, calc_base_t base, calc_width_t width,
                       int sign, calc_value_str_t *out) {
  out->base     = base;
  out->negative = 0;
  out->len      = 0;
  out->digits[0] = '\0';

  uint64_t u = calc_mask(value, width);

  /* DEC + signed: if the high bit of `width` is set, we display as a
   * negative two's-complement number with a leading '-'. */
  if (base == CALC_BASE_DEC && sign && width != CALC_W64) {
    uint64_t top = (uint64_t)1 << ((int)width - 1);
    if (u & top) {
      out->negative = 1;
      uint64_t mask = ((uint64_t)1 << (int)width) - 1u;
      u = ((~u) + 1u) & mask;
    }
  } else if (base == CALC_BASE_DEC && sign && width == CALC_W64) {
    if ((int64_t)u < 0) {
      out->negative = 1;
      u = (uint64_t)0 - u;   /* defined: unsigned wraparound */
    }
  }

  /* Render digits in reverse, then flip in-place. */
  int radix = base_radix(base);
  char tmp[CALC_RENDER_MAX_DIGITS];
  int n = 0;
  if (u == 0) {
    tmp[n++] = '0';
  } else {
    while (u > 0 && n < CALC_RENDER_MAX_DIGITS - 1) {
      int d = (int)(u % (uint64_t)radix);
      tmp[n++] = HEX_UPPER[d];
      u /= (uint64_t)radix;
    }
  }
  /* BIN is always rendered at full width — the LED-dot display's whole
   * point is showing every bit's lit/unlit state.  HEX/OCT/DEC trim. */
  if (base == CALC_BASE_BIN) {
    while (n < (int)width && n < CALC_RENDER_MAX_DIGITS - 1)
      tmp[n++] = '0';
  }

  /* Copy reversed into out->digits */
  for (int i = 0; i < n; i++)
    out->digits[i] = tmp[n - 1 - i];
  out->digits[n] = '\0';
  out->len = n;
}

/* Group size and separator per base for grouped (plain-text) rendering. */
static int group_size(calc_base_t base) {
  switch (base) {
  case CALC_BASE_DEC: return 3;
  case CALC_BASE_HEX: return 4;
  case CALC_BASE_OCT: return 3;
  case CALC_BASE_BIN: return 4;
  }
  return 3;
}

static char group_sep(calc_base_t base) {
  return (base == CALC_BASE_DEC) ? ',' : '_';
}

static const char *base_prefix(calc_base_t base) {
  switch (base) {
  case CALC_BASE_HEX: return "0x";
  case CALC_BASE_OCT: return "0o";
  case CALC_BASE_BIN: return "0b";
  case CALC_BASE_DEC: return "";
  }
  return "";
}

int calc_render_grouped(const calc_value_str_t *v, char *buf, int bufsize) {
  if (bufsize <= 0)
    return 0;

  int gs = group_size(v->base);
  char sep = group_sep(v->base);
  const char *prefix = base_prefix(v->base);

  int wpos = 0;
  /* Helper macro to push a char and bound-check. */
#define PUSH(ch) do {                              \
    if (wpos < bufsize - 1) buf[wpos++] = (ch);    \
  } while (0)

  if (v->negative)
    PUSH('-');
  while (*prefix)
    PUSH(*prefix++);

  /* Walk digits left-to-right; insert separator before positions where
   * the remaining digit count is a positive multiple of group size. */
  int total = v->len;
  for (int i = 0; i < total; i++) {
    int remaining = total - i;
    if (i > 0 && (remaining % gs) == 0)
      PUSH(sep);
    PUSH(v->digits[i]);
  }

  if (wpos < bufsize)
    buf[wpos] = '\0';
  else
    buf[bufsize - 1] = '\0';
  return wpos;
#undef PUSH
}
