/*
 * calc_state.c — programmer's calc state machine
 *
 * Pure: no syscalls, no IO.  Exercised by tests/host/test_calc_state.c.
 */

#include "calc.h"

uint64_t calc_mask(int64_t v, calc_width_t width) {
  if (width == CALC_W64)
    return (uint64_t)v;
  uint64_t m = ((uint64_t)1 << (int)width) - 1u;
  return (uint64_t)v & m;
}

/* Sign-extend the low `width` bits of v to a full int64.  Used when the
 * state's `sign` flag means "interpret display as two's complement". */
static int64_t sign_extend(int64_t v, calc_width_t width) {
  if (width == CALC_W64)
    return v;
  uint64_t m = ((uint64_t)1 << (int)width) - 1u;
  uint64_t u = (uint64_t)v & m;
  uint64_t top = (uint64_t)1 << ((int)width - 1);
  if (u & top)
    return (int64_t)(u | ~m);
  return (int64_t)u;
}

/* Apply width mask to an int64; returns the masked value as int64.
 * The masked value is the canonical bit-pattern stored in display/accum. */
static int64_t mask64(int64_t v, calc_width_t width) {
  return (int64_t)calc_mask(v, width);
}

static int digit_valid(calc_base_t base, int digit) {
  switch (base) {
  case CALC_BASE_BIN: return digit >= 0 && digit < 2;
  case CALC_BASE_OCT: return digit >= 0 && digit < 8;
  case CALC_BASE_DEC: return digit >= 0 && digit < 10;
  case CALC_BASE_HEX: return digit >= 0 && digit < 16;
  }
  return 0;
}

static int base_radix(calc_base_t base) {
  switch (base) {
  case CALC_BASE_BIN: return 2;
  case CALC_BASE_OCT: return 8;
  case CALC_BASE_DEC: return 10;
  case CALC_BASE_HEX: return 16;
  }
  return 10;
}

void calc_init(calc_state_t *s) {
  s->display = 0;
  s->accum   = 0;
  s->ans     = 0;
  s->mem     = 0;
  s->pending = CALC_OP_NONE;
  s->entry   = 0;
  s->base    = CALC_BASE_DEC;
  s->width   = CALC_W32;
  s->sign    = 1;          /* signed by default — matches programmer expectation */
  s->err     = CALC_ERR_NONE;
}

void calc_input_digit(calc_state_t *s, int digit) {
  if (!digit_valid(s->base, digit))
    return;
  s->err = CALC_ERR_NONE;
  if (!s->entry) {
    s->display = 0;
    s->entry = 1;
  }
  int radix = base_radix(s->base);
  /* Operate in unsigned space so we don't trip C's signed overflow UB
   * when digits push past INT64_MAX before the width mask trims them. */
  uint64_t cur = calc_mask(s->display, s->width);
  /* In DEC signed mode, entry is positive — sign is applied via negate
   * key.  Other bases are bit patterns, no sign concept during entry. */
  uint64_t next = cur * (uint64_t)radix + (uint64_t)digit;
  s->display = mask64((int64_t)next, s->width);
}

static int64_t apply_op(calc_state_t *s, int64_t a, int64_t b, calc_op_t op) {
  /* For arithmetic in signed mode we want the user's intuition (signed
   * overflow wraps within the chosen width).  Since C signed overflow
   * is UB, do all arithmetic in uint64 and let the width mask trim. */
  uint64_t ua = calc_mask(a, s->width);
  uint64_t ub = calc_mask(b, s->width);
  uint64_t r = 0;
  switch (op) {
  case CALC_OP_ADD: r = ua + ub; break;
  case CALC_OP_SUB: r = ua - ub; break;
  case CALC_OP_MUL: r = ua * ub; break;
  case CALC_OP_DIV:
  case CALC_OP_MOD: {
    if (ub == 0) {
      s->err = CALC_ERR_DIV0;
      return 0;
    }
    /* Signed division: sign-extend operands first.  Watch for the one
     * undefined case (INT64_MIN / -1) — we wrap it to 0 like other
     * width-overflow results. */
    if (s->sign) {
      int64_t sa = sign_extend(a, s->width);
      int64_t sb = sign_extend(b, s->width);
      int64_t sr;
      if (s->width == CALC_W64 && sa == (int64_t)0x8000000000000000LL && sb == -1)
        sr = 0;
      else
        sr = (op == CALC_OP_DIV) ? (sa / sb) : (sa % sb);
      r = (uint64_t)sr;
    } else {
      r = (op == CALC_OP_DIV) ? (ua / ub) : (ua % ub);
    }
    break;
  }
  case CALC_OP_AND: r = ua & ub; break;
  case CALC_OP_OR:  r = ua | ub; break;
  case CALC_OP_XOR: r = ua ^ ub; break;
  case CALC_OP_SHL: {
    /* Clamp count to [0, width-1]; out-of-range shift -> 0. */
    uint64_t n = ub;
    if (n >= (uint64_t)s->width) {
      r = 0;
    } else {
      r = ua << n;
    }
    break;
  }
  case CALC_OP_SHR: {
    uint64_t n = ub;
    if (n >= (uint64_t)s->width) {
      /* Logical: 0.  Arithmetic on negative: -1; on non-negative: 0. */
      if (s->sign) {
        int64_t sa = sign_extend(a, s->width);
        r = (sa < 0) ? (uint64_t)-1 : 0;
      } else {
        r = 0;
      }
    } else if (s->sign) {
      int64_t sa = sign_extend(a, s->width);
      r = (uint64_t)(sa >> n);
    } else {
      r = ua >> n;
    }
    break;
  }
  case CALC_OP_NONE:
  default:
    r = ub;  /* shouldn't reach here */
    break;
  }
  return mask64((int64_t)r, s->width);
}

void calc_input_op(calc_state_t *s, calc_op_t op) {
  if (s->err != CALC_ERR_NONE)
    return;
  if (s->pending != CALC_OP_NONE && s->entry) {
    int64_t r = apply_op(s, s->accum, s->display, s->pending);
    if (s->err != CALC_ERR_NONE) {
      s->display = 0;
      s->pending = CALC_OP_NONE;
      s->entry = 0;
      return;
    }
    s->display = r;
  }
  s->accum = s->display;
  s->pending = op;
  s->entry = 0;
}

void calc_input_equals(calc_state_t *s) {
  if (s->err != CALC_ERR_NONE)
    return;
  if (s->pending != CALC_OP_NONE) {
    int64_t r = apply_op(s, s->accum, s->display, s->pending);
    if (s->err != CALC_ERR_NONE) {
      s->display = 0;
      s->pending = CALC_OP_NONE;
      s->entry = 0;
      return;
    }
    s->display = r;
    s->pending = CALC_OP_NONE;
  }
  s->ans = s->display;
  s->entry = 0;
}

void calc_input_clear_entry(calc_state_t *s) {
  s->display = 0;
  s->entry = 0;
  s->err = CALC_ERR_NONE;
}

void calc_input_all_clear(calc_state_t *s) {
  s->display = 0;
  s->accum = 0;
  s->pending = CALC_OP_NONE;
  s->entry = 0;
  s->err = CALC_ERR_NONE;
  /* width/base/sign/ans deliberately preserved */
}

void calc_input_negate(calc_state_t *s) {
  if (s->err != CALC_ERR_NONE)
    return;
  uint64_t u = calc_mask(s->display, s->width);
  /* Two's-complement negate: ~u + 1, then mask. */
  uint64_t neg = (~u + 1u);
  s->display = mask64((int64_t)neg, s->width);
}

void calc_input_not(calc_state_t *s) {
  if (s->err != CALC_ERR_NONE)
    return;
  uint64_t u = calc_mask(s->display, s->width);
  s->display = mask64((int64_t)~u, s->width);
}

void calc_input_backspace(calc_state_t *s) {
  if (!s->entry)
    return;
  uint64_t u = calc_mask(s->display, s->width);
  /* Strip the lowest digit in the current base (truncate division). */
  if (s->sign && s->base == CALC_BASE_DEC) {
    int64_t sv = sign_extend(s->display, s->width);
    if (sv < 0) sv = -sv;
    sv /= base_radix(s->base);
    s->display = mask64(sv, s->width);
  } else {
    u /= (uint64_t)base_radix(s->base);
    s->display = mask64((int64_t)u, s->width);
  }
}

void calc_set_base(calc_state_t *s, calc_base_t b) {
  s->base = b;
  /* Switching base does not change the underlying value, but it does end
   * the entry mode: a fresh digit press should start a new number rather
   * than appending in the new base to whatever the user was typing. */
  s->entry = 0;
}

void calc_cycle_width(calc_state_t *s) {
  switch (s->width) {
  case CALC_W8:  s->width = CALC_W16; break;
  case CALC_W16: s->width = CALC_W32; break;
  case CALC_W32: s->width = CALC_W64; break;
  case CALC_W64: s->width = CALC_W8;  break;
  }
  /* Mask both registers to the (possibly narrower) new width. */
  s->display = mask64(s->display, s->width);
  s->accum   = mask64(s->accum,   s->width);
  s->entry = 0;
}

void calc_toggle_sign(calc_state_t *s) {
  s->sign = !s->sign;
}

void calc_mem_clear(calc_state_t *s) {
  s->mem = 0;
}

void calc_mem_recall(calc_state_t *s) {
  s->display = mask64(s->mem, s->width);
  s->entry = 0;
  s->err = CALC_ERR_NONE;
}

void calc_mem_add(calc_state_t *s) {
  uint64_t a = calc_mask(s->mem, s->width);
  uint64_t b = calc_mask(s->display, s->width);
  s->mem = mask64((int64_t)(a + b), s->width);
}

void calc_mem_sub(calc_state_t *s) {
  uint64_t a = calc_mask(s->mem, s->width);
  uint64_t b = calc_mask(s->display, s->width);
  s->mem = mask64((int64_t)(a - b), s->width);
}

void calc_mem_store(calc_state_t *s) {
  s->mem = mask64(s->display, s->width);
}
