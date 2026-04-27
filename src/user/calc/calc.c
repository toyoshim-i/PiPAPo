/*
 * calc.c — PPAP programmer's calculator: TTY frontend
 *
 * Physical-calc UX over an ANSI / VT100 TTY.  Reads single-key input in
 * raw mode, drives the calc_state machine, and renders the display via
 * calc_segdisp (7-seg / LED-dot) with a plain-text fallback.
 *
 * Layout adapts to TTY width:
 *   <  60 cols : minimum view (single column, single-line tape)
 *   60..79     : minimum view with wider value field
 *   >= 80      : tape pane on the right
 *
 * See docs/proposals/calc.md for the full design.
 */

#include "calc.h"
#include "common/errno.h"
#include "common/termios.h"
#include "lib/uclib.h"

/* ── Terminal control ──────────────────────────────────────────────────── */

static struct termios orig_termios;
static int raw_active;

static void term_raw(void) {
  struct termios t;
  ioctl(0, TCGETS, &t);
  uc_memcpy(&orig_termios, &t, sizeof(t));
  /* Disable canonical input, echo, CR-NL translation, software flow.
   * Keep ISIG (Ctrl-C still works) and OPOST (\n -> \r\n on output). */
  t.c_iflag &= ~(ICRNL | IXON);
  t.c_lflag &= ~(ICANON | ECHO);
  ioctl(0, TCSETS, &t);
  raw_active = 1;
}

static void term_restore(void) {
  if (raw_active) {
    ioctl(0, TCSETS, &orig_termios);
    raw_active = 0;
  }
}

static void term_get_size(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
    *rows = ws.ws_row;
    *cols = ws.ws_col;
  } else {
    *rows = 24;
    *cols = 80;
  }
}

static int read_key(void) {
  unsigned char c;
  for (;;) {
    ssize_t n = read(0, &c, 1);
    if (n == 1) return c;
    if (n < 0 && -n == EINTR) continue;
    return -1;
  }
}

static void emit(const char *s) {
  int n = uc_strlen(s);
  write(1, s, n);
}

static void emit_str(const char *s, int n) {
  if (n > 0) write(1, s, n);
}

static void cursor_to(int row, int col) {  /* 0-based */
  /* uc_snprintf reads %d as int32_t — pass 32-bit ints to keep varargs
   * aligned on ia16 (where the default `int` is 16-bit). */
  char buf[32];
  int n = uc_snprintf(buf, sizeof(buf), "\033[%d;%dH",
                      (int32_t)(row + 1), (int32_t)(col + 1));
  emit_str(buf, n);
}

static void clear_screen(void) { emit("\033[2J\033[H"); }
static void cursor_hide(void)  { emit("\033[?25l"); }
static void cursor_show(void)  { emit("\033[?25h"); }
static void attr_reset(void)   { emit("\033[0m"); }
static void attr_dim(void)     { emit("\033[2m"); }
static void attr_reverse(void) { emit("\033[7m"); }
static void attr_red(void)     { emit("\033[31m"); }
static void attr_yellow(void)  { emit("\033[33m"); }
static void attr_cyan(void)    { emit("\033[36m"); }

/* ── ASCII frame chars ─────────────────────────────────────────────────── */
/* No VT100 alt-charset / SO-SI / Unicode — keep the frame ASCII so it
 * renders on every PPAP TTY (xterm, PicoCalc fb_con, pcxt VGA).  Uses
 * '+' for corners and joints, '-' for horizontals, '|' for verticals. */
#define F_HORZ '-'
#define F_VERT '|'
#define F_CORN '+'   /* corners + T-joints + cross */

/* Emit a run of `n` of the same char. */
static void emit_run(char c, int n) {
  if (n <= 0) return;
  char buf[128];
  if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
  for (int i = 0; i < n; i++) buf[i] = c;
  write(1, buf, n);
}

/* ── Tape (recent operations) ──────────────────────────────────────────── */

#define TAPE_SIZE  16
#define TAPE_LINE  48

static char tape_lines[TAPE_SIZE][TAPE_LINE];
static int  tape_count;     /* <= TAPE_SIZE */
static int  tape_head;      /* next slot to write */

static void tape_clear(void) {
  tape_count = 0;
  tape_head = 0;
  for (int i = 0; i < TAPE_SIZE; i++) tape_lines[i][0] = '\0';
}

static void tape_push(const char *s) {
  uc_strncpy(tape_lines[tape_head], s, TAPE_LINE - 1);
  tape_lines[tape_head][TAPE_LINE - 1] = '\0';
  tape_head = (tape_head + 1) % TAPE_SIZE;
  if (tape_count < TAPE_SIZE) tape_count++;
}

/* Get tape entry by reverse index (0 = newest).  Returns "" past end. */
static const char *tape_get(int rev_idx) {
  if (rev_idx < 0 || rev_idx >= tape_count) return "";
  int idx = (tape_head - 1 - rev_idx + TAPE_SIZE) % TAPE_SIZE;
  return tape_lines[idx];
}

/* ── State -> string helpers ────────────────────────────────────────────── */

static const char *base_label(calc_base_t b) {
  switch (b) {
  case CALC_BASE_DEC: return "DEC";
  case CALC_BASE_HEX: return "HEX";
  case CALC_BASE_OCT: return "OCT";
  case CALC_BASE_BIN: return "BIN";
  }
  return "?";
}

static const char *op_str(calc_op_t op) {
  switch (op) {
  case CALC_OP_ADD: return "+";
  case CALC_OP_SUB: return "-";
  case CALC_OP_MUL: return "*";
  case CALC_OP_DIV: return "/";
  case CALC_OP_MOD: return "%";
  case CALC_OP_AND: return "&";
  case CALC_OP_OR:  return "|";
  case CALC_OP_XOR: return "^";
  case CALC_OP_SHL: return "<<";
  case CALC_OP_SHR: return ">>";
  case CALC_OP_NONE: default: return "";
  }
}

/* ── Drawing ───────────────────────────────────────────────────────────── */

/* Layout positions are *frame-inclusive*: r_top is the top border row,
 * r_bot is the bottom border row, divider rows are between sections.
 *
 * Wide layout (>= 80 cols):
 *   col 0          -> left border
 *   col col_div    -> vertical divider between main and tape pane
 *   col cols-1     -> right border
 *   main inner cols: 1 .. col_div-1
 *   tape inner cols: col_div+1 .. cols-2
 *
 * Narrow layout (< 80 cols):
 *   col_div = -1, no tape pane.
 *
 * Inline tape row (r_tape) only present when there is no side pane. */
typedef struct {
  int rows, cols;
  int col_div;        /* col index of vertical divider (-1 if no tape pane) */
  int main_inner_w;   /* cols available for content in main column (no border) */
  int tape_inner_w;   /* cols available for content in tape pane (0 if narrow) */
  int disp_rows;      /* 1 (plain text fallback) or 3 (7-seg) */
  int show_inline_tape;
  /* Row positions (0-based): */
  int r_top;
  int r_status;
  int r_d1;
  int r_disp;         /* first row of display content; spans disp_rows rows */
  int r_d2;
  int r_ans;
  int r_d3;
  int r_tape;         /* inline tape row (only if show_inline_tape) */
  int r_d4;           /* divider above keys */
  int r_keys;         /* first of two keys rows */
  int r_bot;
} layout_t;

/* Compute layout for the current TTY size and current display-row count. */
static void compute_layout(layout_t *L, int disp_rows) {
  term_get_size(&L->rows, &L->cols);
  if (L->cols < 1)  L->cols = 80;
  if (L->rows < 8)  L->rows = 24;
  L->disp_rows = disp_rows;

  if (L->cols >= 80) {
    int tape_w = 28;
    if (tape_w > L->cols / 3) tape_w = L->cols / 3;
    L->col_div       = L->cols - tape_w - 1;
    L->main_inner_w  = L->col_div - 1;
    L->tape_inner_w  = L->cols - L->col_div - 2;
  } else {
    L->col_div       = -1;
    L->main_inner_w  = L->cols - 2;
    L->tape_inner_w  = 0;
  }

  /* Show inline tape only when there's no side pane and the screen has
   * room for the extra two rows (divider + tape line). */
  L->show_inline_tape = (L->col_div < 0);

  /* Vertically pack the main column.  If the screen is too short, drop
   * the inline tape first. */
  int row = 0;
  L->r_top    = row++;
  L->r_status = row++;
  L->r_d1     = row++;
  L->r_disp   = row;       row += disp_rows;
  L->r_d2     = row++;
  L->r_ans    = row++;
  if (L->show_inline_tape) {
    L->r_d3   = row++;
    L->r_tape = row++;
  } else {
    L->r_d3   = -1;
    L->r_tape = -1;
  }
  L->r_d4     = row++;
  L->r_keys   = row;       row += 3;
  L->r_bot    = row;
  /* Total height needed = r_bot + 1.  If it overflows L->rows, drop the
   * inline tape (frees 2 rows).  If still overflowing, we just clip. */
  if (L->r_bot + 1 > L->rows && L->show_inline_tape) {
    L->show_inline_tape = 0;
    /* Recompute row positions without inline tape. */
    row = 0;
    L->r_top = row++;
    L->r_status = row++;
    L->r_d1 = row++;
    L->r_disp = row;       row += disp_rows;
    L->r_d2 = row++;
    L->r_ans = row++;
    L->r_d3 = -1;
    L->r_tape = -1;
    L->r_d4 = row++;
    L->r_keys = row;       row += 2;
    L->r_bot = row;
  }
}

/* ── Frame drawing ─────────────────────────────────────────────────────── */

/* Draw a full-width horizontal bar (top or bottom border).  Both ends
 * are corners, and the tape-divider column gets a joint glyph so the
 * vertical attaches visually.  All chars are ASCII '+' / '-'. */
static void draw_hbar_full(int row, const layout_t *L) {
  cursor_to(row, 0);
  char c[2];
  c[0] = F_CORN; write(1, c, 1);
  if (L->col_div > 0) {
    emit_run(F_HORZ, L->col_div - 1);
    c[0] = F_CORN; write(1, c, 1);
    emit_run(F_HORZ, L->cols - L->col_div - 2);
  } else {
    emit_run(F_HORZ, L->cols - 2);
  }
  c[0] = F_CORN; write(1, c, 1);
}

/* Draw a section divider that spans only the main column.  The right
 * end is the joint to the tape divider (when present) or the right
 * border (when narrow). */
static void draw_hbar_main(int row, const layout_t *L) {
  cursor_to(row, 0);
  char c[2];
  c[0] = F_CORN; write(1, c, 1);
  if (L->col_div > 0) {
    emit_run(F_HORZ, L->col_div - 1);
    c[0] = F_CORN; write(1, c, 1);
  } else {
    emit_run(F_HORZ, L->cols - 2);
    c[0] = F_CORN; write(1, c, 1);
  }
}

/* Draw the full frame: vertical sides + tape divider first, then
 * horizontal borders and section dividers on top.  Drawing in this
 * order lets the dividers' corner glyphs naturally overwrite the
 * verticals at intersection points without per-row special cases. */
static void draw_frame(const layout_t *L) {
  /* Vertical sides + tape divider for every interior row. */
  for (int r = L->r_top + 1; r < L->r_bot; r++) {
    cursor_to(r, 0);            write(1, "|", 1);
    cursor_to(r, L->cols - 1);  write(1, "|", 1);
    if (L->col_div > 0) {
      cursor_to(r, L->col_div); write(1, "|", 1);
    }
  }

  /* Top + bottom borders. */
  draw_hbar_full(L->r_top, L);
  draw_hbar_full(L->r_bot, L);

  /* Section dividers stop at the tape divider so the pane stays whole. */
  draw_hbar_main(L->r_d1, L);
  draw_hbar_main(L->r_d2, L);
  if (L->r_d3 >= 0)
    draw_hbar_main(L->r_d3, L);
  draw_hbar_main(L->r_d4, L);
}

/* Move cursor to the inner-content position (row, col) inside the main
 * column.  col is 0-based within main_inner_w. */
static void cursor_main(int row, int col) {
  cursor_to(row, 1 + col);
}
/* Same for the tape pane (when present). */
static void cursor_tape(const layout_t *L, int row, int col) {
  cursor_to(row, L->col_div + 1 + col);
}

static void draw_status(const calc_state_t *s, const layout_t *L,
                        int m_prefix) {
  cursor_main(L->r_status, 0);
  attr_reset();
  attr_cyan();
  char buf[96];
  int n = uc_snprintf(buf, sizeof(buf), "%s  W=%u  %s",
                      base_label(s->base), (uint32_t)s->width,
                      s->sign ? "signed" : "unsigned");
  if (n > L->main_inner_w) n = L->main_inner_w;
  emit_str(buf, n);
  int used = n;
  if (s->pending != CALC_OP_NONE && used + 14 <= L->main_inner_w) {
    n = uc_snprintf(buf, sizeof(buf), "  [pending %s]", op_str(s->pending));
    if (n > L->main_inner_w - used) n = L->main_inner_w - used;
    emit_str(buf, n);
    used += n;
  }
  if (m_prefix && used + 6 <= L->main_inner_w) {
    emit("  [M..]");
    used += 7;
  }
  if (s->err == CALC_ERR_DIV0 && used + 22 <= L->main_inner_w) {
    attr_red();
    emit("  ERR: divide by zero");
  }
  attr_reset();
}

static void draw_display(const calc_state_t *s, const layout_t *L,
                         int force_text) {
  calc_value_str_t v;
  calc_render_value(s->display, s->base, s->width, s->sign, &v);
  calc_disp_t d;
  calc_segdisp_render(&v, L->main_inner_w, force_text, &d);

  if (d.used_seg) attr_yellow();
  for (int r = 0; r < d.line_count && r < L->disp_rows; r++) {
    cursor_main(L->r_disp + r, 0);
    emit_str(d.lines[r], d.visible_w);
  }
  attr_reset();
}

static void draw_ans(const calc_state_t *s, const layout_t *L) {
  cursor_main(L->r_ans, 0);
  attr_dim();
  char buf[CALC_DISP_MAX_LINE];
  calc_value_str_t v;

  /* "ans = X    M = Y" — both rendered in the current base. */
  calc_render_value(s->ans, s->base, s->width, s->sign, &v);
  int wa = calc_render_grouped(&v, buf, sizeof(buf));
  emit("ans = ");
  int used = 6;
  int budget = L->main_inner_w - used;
  if (wa > budget) wa = budget;
  if (wa > 0) { emit_str(buf, wa); used += wa; }

  if (used + 8 <= L->main_inner_w) {
    calc_render_value(s->mem, s->base, s->width, s->sign, &v);
    int wm = calc_render_grouped(&v, buf, sizeof(buf));
    emit("    M = ");
    used += 8;
    int rem = L->main_inner_w - used;
    if (wm > rem) wm = rem;
    if (wm > 0) emit_str(buf, wm);
  }
  attr_reset();
}

/* Width of every hint cell: 6 chars label + 1 space separator. */
#define HINT_W   6
#define HINT_GAP 1

/* Emit one key-hint cell padded to HINT_W chars + HINT_GAP space.  The
 * first character is drawn in reverse video (the hotkey it represents),
 * the rest is dim descriptive text.  Labels longer than HINT_W are
 * truncated. */
static void emit_hint(const char *label) {
  if (!label || !*label) return;
  attr_reset();
  attr_reverse();
  char c[2] = { label[0], 0 };
  write(1, c, 1);
  attr_reset();
  attr_dim();
  int rest = uc_strlen(label + 1);
  int max_rest = HINT_W - 1;
  if (rest > max_rest) rest = max_rest;
  if (rest > 0) emit_str(label + 1, rest);
  /* Pad label to HINT_W. */
  int pad = HINT_W - 1 - rest;
  for (int i = 0; i < pad; i++) write(1, " ", 1);
  /* Cell separator. */
  for (int i = 0; i < HINT_GAP; i++) write(1, " ", 1);
}

/* Draw the three key-hint rows: bases, state toggles, actions.
 * All cells share a fixed 7-col slot (6 label + 1 gap) so the columns
 * line up across rows. */
static void draw_keys(const layout_t *L) {
  /* Row 1 — base selection (4 cells, 28 cols). */
  cursor_main(L->r_keys, 0);
  attr_dim();
  emit_hint("DEC");
  emit_hint("HEX");
  emit_hint("OCT");
  emit_hint("BIN");
  attr_reset();

  /* Row 2 — state toggles + memory prefix (4 cells, 28 cols). */
  cursor_main(L->r_keys + 1, 0);
  attr_dim();
  emit_hint("WIDTH");
  emit_hint("SIGN");
  emit_hint("NEG");
  emit_hint("MEM..");
  attr_reset();

  /* Row 3 — actions (5 cells, 35 cols). */
  cursor_main(L->r_keys + 2, 0);
  attr_dim();
  emit_hint("CLR");
  emit_hint("RESET");
  emit_hint("TOGGLE");
  emit_hint("QUIT");
  emit_hint("?HELP");
  attr_reset();
}

static void draw_tape_inline(const layout_t *L) {
  if (!L->show_inline_tape) return;
  cursor_main(L->r_tape, 0);
  attr_dim();
  emit("tape: ");
  const char *latest = tape_get(0);
  if (*latest) {
    int n = uc_strlen(latest);
    int budget = L->main_inner_w - 6;
    if (n > budget) n = budget;
    if (n > 0) emit_str(latest, n);
  }
  attr_reset();
}

static void draw_tape_pane(const layout_t *L) {
  if (L->col_div < 0) return;
  attr_dim();
  /* Header: " TAPE" inside the pane on the top inner row. */
  cursor_tape(L, L->r_top + 1, 0);
  emit("TAPE");
  int max_lines = L->r_bot - L->r_top - 2;
  if (max_lines > tape_count) max_lines = tape_count;
  for (int i = 0; i < max_lines; i++) {
    cursor_tape(L, L->r_top + 2 + i, 0);
    const char *t = tape_get(i);
    int n = uc_strlen(t);
    if (n > L->tape_inner_w) n = L->tape_inner_w;
    if (n > 0) emit_str(t, n);
  }
  attr_reset();
}

/* Static table of help lines; rendered into the inner frame area when
 * the user presses '?'.  Lists the keys that aren't already obvious
 * from the bottom-row hint footer (operators, editing, hex digits). */
static const char *HELP_LINES[] = {
    "0-9 a-f     digit input (a-f only in HEX)",
    "+ - * / %   arithmetic",
    "& | ^ ~     bitwise AND / OR / XOR / NOT",
    "< >         shift left / shift right",
    "= Enter     evaluate pending op",
    "Backspace   delete last digit",
    "",
    "D H O B     switch to DEC / HEX / OCT / BIN",
    "W S N       cycle WIDTH, toggle SIGN, NEGate",
    "C R         CLR entry, RESET all + tape",
    "T           TOGGLE 7-seg / plain text",
    "M then ...  c clear  r recall  s store  + add  - sub",
    "Q (or q)    QUIT",
    "",
    "Press any key to return.",
};
#define HELP_LINES_N ((int)(sizeof(HELP_LINES) / sizeof(HELP_LINES[0])))

static void draw_help(const layout_t *L) {
  cursor_main(L->r_status, 0);
  attr_reset();
  attr_cyan();
  emit("HELP");
  attr_reset();

  attr_dim();
  /* Help body fills the inner area from r_disp down through r_keys+2. */
  int avail = L->r_bot - L->r_disp;
  int n = HELP_LINES_N;
  if (n > avail) n = avail;
  for (int i = 0; i < n; i++) {
    cursor_main(L->r_disp + i, 0);
    int len = uc_strlen(HELP_LINES[i]);
    if (len > L->main_inner_w) len = L->main_inner_w;
    if (len > 0) emit_str(HELP_LINES[i], len);
  }
  attr_reset();
}

static void draw_all(const calc_state_t *s, int force_text, int help_shown,
                     int m_prefix) {
  /* Pre-render the display once so we know whether it's 1 row (plain
   * fallback) or 3 rows (7-seg).  Layout depends on this. */
  layout_t L_probe;
  compute_layout(&L_probe, 3);
  calc_value_str_t v;
  calc_render_value(s->display, s->base, s->width, s->sign, &v);
  calc_disp_t d;
  calc_segdisp_render(&v, L_probe.main_inner_w, force_text, &d);

  layout_t L;
  compute_layout(&L, d.line_count);

  clear_screen();
  cursor_hide();
  draw_frame(&L);

  if (help_shown) {
    draw_help(&L);
  } else {
    draw_status(s, &L, m_prefix);
    draw_display(s, &L, force_text);
    draw_ans(s, &L);
    draw_tape_inline(&L);
    draw_keys(&L);
    draw_tape_pane(&L);
  }
}

/* ── Tape entry construction ───────────────────────────────────────────── */

static void log_op(const calc_state_t *s, calc_op_t op,
                   int64_t before, int64_t after) {
  /* Format: "<op> <operand> = <after>" (operand is the just-typed value;
   * "before" is the running accumulator before folding).  When op is
   * CALC_OP_NONE this represents an = press, which we render as a single
   * "= <result>" line. */
  char buf[TAPE_LINE];
  calc_value_str_t v;
  char operand[32], result[32];
  calc_render_value(before, s->base, s->width, s->sign, &v);
  calc_render_grouped(&v, operand, sizeof(operand));
  calc_render_value(after, s->base, s->width, s->sign, &v);
  calc_render_grouped(&v, result, sizeof(result));

  if (op == CALC_OP_NONE) {
    uc_snprintf(buf, sizeof(buf), "= %s", result);
  } else {
    uc_snprintf(buf, sizeof(buf), "%s %s = %s",
                op_str(op), operand, result);
  }
  tape_push(buf);
}

static void log_seed(const calc_state_t *s, int64_t value) {
  /* First operand of a fresh chain.  Just push the value bare. */
  char buf[TAPE_LINE];
  calc_value_str_t v;
  char text[32];
  calc_render_value(value, s->base, s->width, s->sign, &v);
  calc_render_grouped(&v, text, sizeof(text));
  uc_snprintf(buf, sizeof(buf), "  %s", text);
  tape_push(buf);
}

/* ── Key dispatch ──────────────────────────────────────────────────────── */

/* Returns 1 if calc should quit. */
static int handle_key(calc_state_t *s, int key, int *force_text,
                      int *help_shown, int *m_prefix) {
  /* Help overlay: `?` toggles on; any other key dismisses (and is then
   * processed normally below). */
  if (*help_shown) {
    *help_shown = 0;
    if (key == '?') return 0;  /* toggled off; don't process further */
    /* fall through and handle the dismissing key as a normal command */
  }

  /* M-prefix (memory operations): after pressing 'M', the next key picks
   * the operation.  Lowercase second-stroke matches the broader scheme
   * (uppercase = single command, lowercase = sub-command or hex). */
  if (*m_prefix) {
    *m_prefix = 0;
    switch (key) {
      case 'c': calc_mem_clear(s);  return 0;
      case 'r': calc_mem_recall(s); return 0;
      case 's': calc_mem_store(s);  return 0;
      case '+': calc_mem_add(s);    return 0;
      case '-': calc_mem_sub(s);    return 0;
      default:  break;  /* unknown second stroke — cancel and fall through */
    }
  }
  /* Key convention (strict):
   *   - decimal digits '0'..'9'  : digit input (filtered by base)
   *   - lowercase 'a'..'f'       : hex digit input (HEX mode only)
   *   - uppercase 'A'..'Z'       : commands — exactly the first char of
   *                                each reverse-video hint label
   *
   * Lowercase letters that are not hex digits (h, n, o, q, r, s, t, w)
   * are deliberately NOT accepted as commands — uppercase only.  This
   * keeps the rule symmetric and easy to remember: "all commands are
   * uppercase, all hex digit input is lowercase". */
  switch (key) {

  /* --- decimal digit input --- */
  case '0': case '1': case '2': case '3': case '4':
  case '5': case '6': case '7': case '8': case '9':
    calc_input_digit(s, key - '0');
    return 0;

  /* --- hex digit input (HEX mode only) --- */
  case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
    if (s->base == CALC_BASE_HEX)
      calc_input_digit(s, 10 + (key - 'a'));
    return 0;

  /* --- commands (uppercase only) --- */
  case 'D': calc_set_base(s, CALC_BASE_DEC); return 0;
  case 'H': calc_set_base(s, CALC_BASE_HEX); return 0;
  case 'O': calc_set_base(s, CALC_BASE_OCT); return 0;
  case 'B': calc_set_base(s, CALC_BASE_BIN); return 0;
  case 'C': calc_input_clear_entry(s); return 0;
  case 'R': calc_input_all_clear(s); tape_clear(); return 0;
  case 'W': calc_cycle_width(s); return 0;
  case 'S': calc_toggle_sign(s); return 0;
  case 'N': calc_input_negate(s); return 0;
  case 'T': *force_text = !*force_text; return 0;
  case 'M': *m_prefix = 1; return 0;            /* memory 2-stroke prefix */
  case 'Q': case 'q':            /* lowercase q kept as a quit escape hatch */
  case 4:  /* Ctrl-D */
    return 1;
  case '?':
    *help_shown = 1;
    return 0;

  /* --- arithmetic / bitwise ops --- */
  case '+': case '-': case '*': case '/': case '%':
  case '&': case '|': case '^':
  case '<': case '>': {
    calc_op_t op = CALC_OP_NONE;
    switch (key) {
    case '+': op = CALC_OP_ADD; break;
    case '-': op = CALC_OP_SUB; break;
    case '*': op = CALC_OP_MUL; break;
    case '/': op = CALC_OP_DIV; break;
    case '%': op = CALC_OP_MOD; break;
    case '&': op = CALC_OP_AND; break;
    case '|': op = CALC_OP_OR;  break;
    case '^': op = CALC_OP_XOR; break;
    case '<': op = CALC_OP_SHL; break;
    case '>': op = CALC_OP_SHR; break;
    }
    calc_op_t old_pending = s->pending;
    int64_t operand = s->display;  /* value typed before this op press */
    calc_input_op(s, op);
    if (old_pending != CALC_OP_NONE) {
      /* The just-committed op folded `accum` and `operand` into display. */
      log_op(s, old_pending, operand, s->display);
    } else {
      /* First op of a chain — log the seed value. */
      log_seed(s, operand);
    }
    return 0;
  }

  case '=':
  case '\r':
  case '\n': {
    int64_t before_disp = s->display;
    calc_op_t old_pending = s->pending;
    calc_input_equals(s);
    if (old_pending != CALC_OP_NONE) {
      log_op(s, old_pending, before_disp, s->display);
    }
    log_op(s, CALC_OP_NONE, 0, s->display);  /* "= result" cap */
    return 0;
  }

  /* --- editing --- */
  case 127: case 8:  /* Backspace */
    calc_input_backspace(s);
    return 0;
  case '~':
    calc_input_not(s);
    return 0;
  }
  return 0;
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  calc_state_t s;
  calc_init(&s);
  tape_clear();
  int force_text = 0;
  int help_shown = 0;
  int m_prefix   = 0;

  term_raw();
  draw_all(&s, force_text, help_shown, m_prefix);

  for (;;) {
    int key = read_key();
    if (key < 0) break;
    int quit = handle_key(&s, key, &force_text, &help_shown, &m_prefix);
    draw_all(&s, force_text, help_shown, m_prefix);
    if (quit) break;
  }

  /* Restore screen state: cursor visible, normal attrs, then move the
   * cursor below the frame so the shell prompt appears on a fresh line. */
  cursor_show();
  attr_reset();
  int rows, cols;
  term_get_size(&rows, &cols);
  cursor_to(rows - 1, 0);
  emit("\n");
  term_restore();
  return 0;
}
