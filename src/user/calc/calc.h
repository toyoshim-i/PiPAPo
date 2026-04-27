/*
 * calc.h — PPAP programmer's calculator: shared types and entry points
 *
 * Physical-calc state machine (single accumulator + pending op),
 * integer-only, with width-aware two's-complement masking and base
 * switching across DEC / HEX / OCT / BIN.  See docs/proposals/calc.md.
 *
 * Layered:
 *   calc_state.c    state machine + ops
 *   calc_render.c   value -> digit string with grouping
 *   calc_segdisp.c  7-seg / LED-dot multi-line display + plain fallback
 *   calc.c          TTY frontend (raw mode, key dispatch, drawing)
 *
 * The state, render, and segdisp layers are pure (no syscalls, no IO)
 * so they are exercised by host unit tests.
 */

#ifndef PPAP_USER_CALC_H
#define PPAP_USER_CALC_H

#include <stdint.h>

/* ── State machine ──────────────────────────────────────────────────────── */

typedef enum {
  CALC_BASE_DEC = 0,
  CALC_BASE_HEX,
  CALC_BASE_OCT,
  CALC_BASE_BIN,
} calc_base_t;

typedef enum {
  CALC_W8  = 8,
  CALC_W16 = 16,
  CALC_W32 = 32,
  CALC_W64 = 64,
} calc_width_t;

typedef enum {
  CALC_OP_NONE = 0,
  CALC_OP_ADD,
  CALC_OP_SUB,
  CALC_OP_MUL,
  CALC_OP_DIV,
  CALC_OP_MOD,
  CALC_OP_AND,
  CALC_OP_OR,
  CALC_OP_XOR,
  CALC_OP_SHL,
  CALC_OP_SHR,
} calc_op_t;

typedef enum {
  CALC_ERR_NONE = 0,
  CALC_ERR_DIV0,
} calc_err_t;

typedef struct {
  int64_t      display;   /* value being shown / typed                       */
  int64_t      accum;     /* left operand of pending op                      */
  int64_t      ans;       /* last committed result                           */
  int64_t      mem;       /* memory register (M+, M-, MR, MC, MS)            */
  calc_op_t    pending;
  int          entry;     /* nonzero while user is typing into display       */
  calc_base_t  base;
  calc_width_t width;
  int          sign;      /* nonzero = signed display interpretation         */
  calc_err_t   err;
} calc_state_t;

void calc_init(calc_state_t *s);

/* Append a digit (value 0..15) to the display when valid for the current
 * base; otherwise no-op.  Starts a fresh entry if entry == 0. */
void calc_input_digit(calc_state_t *s, int digit);

/* Apply pending op (if any), then store new pending op. */
void calc_input_op(calc_state_t *s, calc_op_t op);

/* Apply pending op and clear it; updates ans. */
void calc_input_equals(calc_state_t *s);

/* Clear current entry: display = 0, entry = 0, err = NONE.  Pending op
 * and accumulator are preserved. */
void calc_input_clear_entry(calc_state_t *s);

/* All clear: reset everything except width / base / sign preferences. */
void calc_input_all_clear(calc_state_t *s);

/* Negate the current display value (two's complement at current width). */
void calc_input_negate(calc_state_t *s);

/* Bitwise NOT of the current display value, masked to current width. */
void calc_input_not(calc_state_t *s);

/* Delete the last entered digit (only meaningful while entry == 1). */
void calc_input_backspace(calc_state_t *s);

void calc_set_base(calc_state_t *s, calc_base_t b);
void calc_cycle_width(calc_state_t *s);   /* 8 -> 16 -> 32 -> 64 -> 8 */
void calc_toggle_sign(calc_state_t *s);

/* Memory register ops.  All apply width masking so mem stays in range. */
void calc_mem_clear(calc_state_t *s);     /* MC: mem = 0 */
void calc_mem_recall(calc_state_t *s);    /* MR: display = mem; ends entry */
void calc_mem_add(calc_state_t *s);       /* M+: mem += display, masked */
void calc_mem_sub(calc_state_t *s);       /* M-: mem -= display, masked */
void calc_mem_store(calc_state_t *s);     /* MS: mem = display */

/* Apply mask for `width` to `v` and return the result as an unsigned value
 * suitable for digit rendering.  Top-bit handling for signed display is the
 * renderer's job, not this helper's. */
uint64_t calc_mask(int64_t v, calc_width_t width);

/* ── Value rendering (calc_render.c) ────────────────────────────────────── */

/* Maximum digits across all bases / widths:
 *   BIN width=64 -> 64 digits.  Add slack for a leading sign. */
#define CALC_RENDER_MAX_DIGITS 80

typedef struct {
  char         digits[CALC_RENDER_MAX_DIGITS]; /* raw digits, NUL-terminated */
  int          len;                            /* digit count, excludes sign */
  int          negative;                       /* nonzero = leading '-'      */
  calc_base_t  base;
} calc_value_str_t;

/* Render `value` as a digit string in `base`, masked to `width`, with
 * signed/unsigned interpretation per `sign`.  `out->digits` always has
 * at least one digit ("0" for zero values) and is uppercase for HEX. */
void calc_render_value(int64_t value, calc_base_t base, calc_width_t width,
                       int sign, calc_value_str_t *out);

/* Format a value as plain text with grouping separators and base prefix:
 *   DEC: "1,234,567"  (commas every 3, optional leading '-')
 *   HEX: "0xFFFF_0000" (underscores every 4)
 *   OCT: "0o37_777_777" (underscores every 3)
 *   BIN: "0b1100_1100" (underscores every 4)
 * Writes up to `bufsize` bytes including NUL.  Returns visible width
 * (excluding NUL).  Output is always NUL-terminated when bufsize > 0. */
int calc_render_grouped(const calc_value_str_t *v, char *buf, int bufsize);

/* ── Segmented display (calc_segdisp.c) ─────────────────────────────────── */

/* Output line capacity.  Width=64 BIN as LED dots needs:
 *   64 cells + 15 group separators + 0 prefix = 79 cells.  Round up. */
#define CALC_DISP_MAX_LINES 3
#define CALC_DISP_MAX_LINE  128

typedef struct {
  char  lines[CALC_DISP_MAX_LINES][CALC_DISP_MAX_LINE];
  int   line_count;   /* 1 for plain text or BIN LED-dot row, 3 for 7-seg */
  int   visible_w;    /* cell width of each line                          */
  int   used_seg;     /* nonzero if 7-seg / LED form was used (vs text)   */
} calc_disp_t;

/* Render `v` for display.
 *
 *   max_cols   layout width budget; the chosen form must fit.
 *   force_text if nonzero, always render plain text (one line, with
 *              base prefix and grouping separators).
 *
 * Selection rules (when force_text == 0):
 *   - BIN: LED-dot row when it fits (1 line); otherwise plain text.
 *   - DEC / HEX / OCT: 7-seg multi-line when it fits (3 lines);
 *                      otherwise plain text.
 *
 * The plain-text fallback always fits CALC_DISP_MAX_LINE for the
 * supported value range (worst case: width=64 BIN ≈ 84 cells incl.
 * "0b" + 15 underscores).  Output is right-aligned within `max_cols`
 * by left-padding with spaces. */
void calc_segdisp_render(const calc_value_str_t *v, int max_cols,
                         int force_text, calc_disp_t *out);

#endif /* PPAP_USER_CALC_H */
