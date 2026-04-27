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
  char buf[32];
  int n = uc_snprintf(buf, sizeof(buf), "\033[%d;%dH", row + 1, col + 1);
  emit_str(buf, n);
}

static void clear_screen(void) { emit("\033[2J\033[H"); }
static void cursor_hide(void)  { emit("\033[?25l"); }
static void cursor_show(void)  { emit("\033[?25h"); }
static void attr_reset(void)   { emit("\033[0m"); }
static void attr_dim(void)     { emit("\033[2m"); }
static void attr_red(void)     { emit("\033[31m"); }
static void attr_yellow(void)  { emit("\033[33m"); }
static void attr_cyan(void)    { emit("\033[36m"); }

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

typedef struct {
  int rows;
  int cols;
  int show_tape_pane;     /* nonzero -> sidebar tape on the right */
  int main_w;             /* width of the main calc column        */
  int tape_w;             /* width of the tape pane (0 if none)   */
} layout_t;

static void compute_layout(layout_t *L) {
  term_get_size(&L->rows, &L->cols);
  if (L->cols < 1)  L->cols = 80;
  if (L->rows < 10) L->rows = 24;

  if (L->cols >= 80) {
    L->show_tape_pane = 1;
    L->tape_w = 28;
    if (L->tape_w > L->cols / 3) L->tape_w = L->cols / 3;
    L->main_w = L->cols - L->tape_w - 1;  /* -1 for divider column */
  } else {
    L->show_tape_pane = 0;
    L->tape_w = 0;
    L->main_w = L->cols;
  }
}

static void draw_status(const calc_state_t *s, const layout_t *L) {
  cursor_to(0, 0);
  attr_reset();
  attr_cyan();
  char buf[64];
  int n = uc_snprintf(buf, sizeof(buf), "%s  W=%u  %s",
                      base_label(s->base), (unsigned)s->width,
                      s->sign ? "signed" : "unsigned");
  emit_str(buf, n);
  if (s->pending != CALC_OP_NONE) {
    n = uc_snprintf(buf, sizeof(buf), "  [pending %s]", op_str(s->pending));
    emit_str(buf, n);
  }
  if (s->err == CALC_ERR_DIV0) {
    attr_red();
    emit("  ERR: divide by zero");
  }
  attr_reset();
}

static void draw_display(const calc_state_t *s, const layout_t *L,
                         int force_text, int row) {
  calc_value_str_t v;
  calc_render_value(s->display, s->base, s->width, s->sign, &v);
  calc_disp_t d;
  calc_segdisp_render(&v, L->main_w, force_text, &d);

  /* Color: lit segments yellow on color-capable TTYs; we just colorise
   * the whole 7-seg / LED block in one shot for simplicity. */
  if (d.used_seg) attr_yellow();
  for (int r = 0; r < d.line_count; r++) {
    cursor_to(row + r, 0);
    emit_str(d.lines[r], d.visible_w);
  }
  attr_reset();
}

static void draw_ans(const calc_state_t *s, const layout_t *L, int row) {
  cursor_to(row, 0);
  attr_dim();
  char buf[CALC_DISP_MAX_LINE];
  /* Render ans in the current base for consistency with the main display. */
  calc_value_str_t v;
  calc_render_value(s->ans, s->base, s->width, s->sign, &v);
  int w = calc_render_grouped(&v, buf, sizeof(buf));
  emit("ans = ");
  emit_str(buf, w);
  attr_reset();
}

static void draw_keys(const layout_t *L, int row) {
  cursor_to(row, 0);
  attr_dim();
  emit("[d]EC [h]EX [o]CT [b]IN  [w]idth [s]ign [n]eg");
  cursor_to(row + 1, 0);
  emit("[c]CE [C]AC  [t]oggle 7seg  [q]uit  [?]help");
  attr_reset();
}

static void draw_tape_inline(const layout_t *L, int row) {
  /* Single-line tape for narrow TTYs.  Show the most recent entry. */
  cursor_to(row, 0);
  attr_dim();
  emit("tape: ");
  const char *latest = tape_get(0);
  if (*latest)
    emit(latest);
  attr_reset();
}

static void draw_tape_pane(const layout_t *L, int top_row, int bottom_row) {
  if (!L->show_tape_pane) return;
  int x = L->main_w + 1;  /* skip divider column */
  attr_dim();
  cursor_to(top_row, x);
  emit("TAPE");
  int max_lines = bottom_row - top_row;
  if (max_lines > tape_count) max_lines = tape_count;
  for (int i = 0; i < max_lines; i++) {
    cursor_to(top_row + 1 + i, x);
    emit(tape_get(i));
  }
  attr_reset();
}

static void draw_divider(const layout_t *L) {
  if (!L->show_tape_pane) return;
  attr_dim();
  for (int r = 0; r < L->rows - 1; r++) {
    cursor_to(r, L->main_w);
    emit("|");
  }
  attr_reset();
}

static void draw_all(const calc_state_t *s, int force_text) {
  layout_t L;
  compute_layout(&L);

  clear_screen();
  cursor_hide();

  draw_status(s, &L);
  draw_display(s, &L, force_text, 2);     /* row 2..4 if 7-seg, else row 2 */
  draw_ans(s, &L, 6);
  draw_tape_inline(&L, 8);                /* always, on every layout */
  draw_keys(&L, L.rows - 3);
  draw_tape_pane(&L, 0, L.rows - 1);
  draw_divider(&L);
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

static int hex_digit_value(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

/* Returns 1 if calc should quit. */
static int handle_key(calc_state_t *s, int key, int *force_text) {
  switch (key) {
  case 'q': case 'Q':
  case 4:  /* Ctrl-D */
    return 1;

  case '?':
    /* Help is implicit in the key hint footer; no overlay yet. */
    return 0;

  /* --- digit input --- */
  case '0': case '1': case '2': case '3': case '4':
  case '5': case '6': case '7': case '8': case '9':
  case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
  case 'A': case 'B': case 'C': case 'D': case 'E': case 'F': {
    /* `b`/`c`/`d`/`f` collide with base / clear / quit hints.  Resolve:
     *   - hex digit only when current base is HEX
     *   - otherwise dispatch as command key (handled below)
     * Fall-through is tricky in C, so we branch here. */
    int v = hex_digit_value(key);
    if (v >= 0 && s->base == CALC_BASE_HEX && v < 16) {
      calc_input_digit(s, v);
      return 0;
    }
    /* Re-dispatch for command keys */
    switch (key) {
    case 'b': calc_set_base(s, CALC_BASE_BIN); return 0;
    case 'c': calc_input_clear_entry(s); return 0;
    case 'd': calc_set_base(s, CALC_BASE_DEC); return 0;
    case 'C': calc_input_all_clear(s); tape_clear(); return 0;
    default:
      /* digits 0..9 always work; A/E/F outside HEX are no-ops */
      if (v >= 0 && v < 10) {
        calc_input_digit(s, v);
      }
      return 0;
    }
  }

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

  case 'n':
    calc_input_negate(s);
    return 0;

  /* --- mode --- */
  case 'h': calc_set_base(s, CALC_BASE_HEX); return 0;
  case 'H': calc_set_base(s, CALC_BASE_HEX); return 0;
  case 'o': calc_set_base(s, CALC_BASE_OCT); return 0;
  case 'O': calc_set_base(s, CALC_BASE_OCT); return 0;
  case 'w': calc_cycle_width(s); return 0;
  case 'W': calc_cycle_width(s); return 0;
  case 's': calc_toggle_sign(s); return 0;
  case 'S': calc_toggle_sign(s); return 0;

  case 't': case 'T':
    *force_text = !*force_text;
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

  term_raw();
  draw_all(&s, force_text);

  for (;;) {
    int key = read_key();
    if (key < 0) break;
    int quit = handle_key(&s, key, &force_text);
    draw_all(&s, force_text);
    if (quit) break;
  }

  cursor_show();
  attr_reset();
  /* Move cursor to bottom so the shell prompt appears on a fresh line. */
  cursor_to(60, 0);
  emit("\n");
  term_restore();
  return 0;
}
