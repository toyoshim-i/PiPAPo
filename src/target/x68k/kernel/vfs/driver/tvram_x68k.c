/*
 * tvram_x68k.c — X68000 TVRAM text console driver via IPL IOCS
 *
 * Implements the shared uart.h console interface (uart_init / uart_putc /
 * uart_getc / uart_rx_avail) on the built-in TVRAM text plane using X68000
 * IOCS calls (TRAP #15).  Stage2 preserves the IPL IOCS handler at vector 47
 * (TRAP #15), so all IOCS function codes work from supervisor mode.
 *
 * IMPORTANT: All IOCS calls take the x68k IOCS guard and mask PPAP's Timer-C
 * scheduler source while leaving the ROM's other required interrupts enabled.
 * IOCS functions are not reentrant — they use a shared work area in low RAM.
 *
 * VT100 escape sequence converter:
 *   X68000 IOCS _B_PUTC has its own escape sequence parser that is NOT
 *   VT100-compatible.  Sending VT100 CSI sequences directly causes the
 *   IOCS parser to enter a bad state → address error crash.  This driver
 *   intercepts ESC sequences, parses them as VT100, and converts supported
 *   sequences into equivalent IOCS calls.  Unsupported sequences are
 *   silently dropped.
 *
 * IOCS functions used:
 *   _B_PUTC    (d0=0x20, d1.w=char)  Output one character to TVRAM console
 *   _B_KEYINP  (d0=0x00)             Dequeue one key (blocks on empty ring)
 *   _B_KEYSNS  (d0=0x01)             Key-ring sense (non-destructive)
 *   _B_COLOR   (d0=0x22, d1.w=code)  Set text colour/attribute for _B_PUTC
 *   _B_LOCATE  (d0=0x23, d1.w=x, d2.w=y)  Set cursor position
 *   _B_CLRST   (d0=0x2A, d1.w=2)     Clear entire screen
 *   _B_CLRST   (d0=0x2A, d1.w=0)     Clear from cursor to end of screen
 *   _B_ERA_AL  (d0=0x2B)             Clear from cursor to end of line
 *   _B_CONSOL  (d0=0x2E)             Query text console geometry (cols/rows)
 */

#include "kernel/vfs/driver/tvram_x68k.h"

#include <stdbool.h>
#include <stdint.h>

#include "kernel/vfs/driver/uart.h"
#include "kernel/vfs/driver/x68k_iocs.h"

/* ── IOCS call wrappers ────────────────────────────────────────────────── */

static inline void iocs_b_clr_st(void) {
  register int32_t d0 asm("d0") = 0x2A;
  register int32_t d1 asm("d1") = 2; /* mode 2 = clear entire screen */
  asm volatile("trap #15" : "+r"(d0) : "r"(d1) : "d2", "a0", "a1", "memory");
}

static inline void iocs_b_clr_ed(void) {
  register int32_t d0 asm("d0") = 0x2A;
  register int32_t d1 asm("d1") =
      0; /* mode 0 = clear cursor to end of screen */
  asm volatile("trap #15" : "+r"(d0) : "r"(d1) : "d2", "a0", "a1", "memory");
}

static inline void iocs_b_clr_al(void) {
  register int32_t d0 asm("d0") = 0x2B; /* _B_ERA_AL — clear cursor to EOL */
  asm volatile("trap #15" : "+r"(d0) : : "d1", "d2", "a0", "a1", "memory");
}

static inline void iocs_b_putc(char c) {
  register int32_t d0 asm("d0") = 0x20;
  register int32_t d1 asm("d1") = (unsigned char)c;
  asm volatile("trap #15" : "+r"(d0) : "r"(d1) : "d2", "a0", "a1", "memory");
}

static inline void iocs_b_locate(int x, int y) {
  register int32_t d0 asm("d0") = 0x23;
  register int32_t d1 asm("d1") = x;
  register int32_t d2 asm("d2") = y;
  asm volatile("trap #15" : "+r"(d0) : "r"(d1), "r"(d2) : "a0", "a1", "memory");
}

/* _B_COLOR — set the text colour/attribute used by subsequent _B_PUTC.  The
 * code selects an X68000 text-palette entry: base 0-3, +4 emphasis, +8
 * reverse. */
static inline void iocs_b_color(int code) {
  register int32_t d0 asm("d0") = 0x22;
  register int32_t d1 asm("d1") = code;
  asm volatile("trap #15" : "+r"(d0) : "r"(d1) : "d2", "a0", "a1", "memory");
}

/* _B_CONSOL — query the current text console geometry.  Called with d1=d2=-1
 * (change nothing); the ROM returns the current display range in d2, packed
 * as (cols-1) in the high word and (rows-1) in the low byte. */
static void iocs_b_consol_query(int *cols, int *rows) {
  register int32_t d0 asm("d0") = 0x2E;
  register int32_t d1 asm("d1") = -1;
  register int32_t d2 asm("d2") = -1;
  asm volatile("trap #15"
               : "+r"(d0), "+r"(d1), "+r"(d2)
               :
               : "a0", "a1", "memory");
  *cols = (int)(((uint32_t)d2 >> 16) & 0xFFFFu) + 1;
  *rows = (int)((uint32_t)d2 & 0xFFu) + 1;
}

static inline int iocs_b_getc(void) {
  register int32_t d0 asm("d0") = 0x00; /* _B_KEYINP — blocking key read */
  asm volatile("trap #15" : "+r"(d0) : : "d1", "d2", "a0", "a1", "memory");
  return d0 & 0xFF;
}

static inline int iocs_b_keysns(void) {
  /* $01 = _B_KEYSNS: non-destructive sense of the IOCS key ring.  Returns 0
   * when the ring is empty, nonzero when a key is queued.  _B_KEYSNS and
   * _B_KEYINP operate on the same ring and the keyboard ISR only enqueues,
   * so under the IOCS mutex "sense nonzero ⇒ _B_KEYINP returns without
   * waiting" holds; uart_getc() depends on that to never block in ROM. */
  register int32_t d0 asm("d0") = 0x01;
  asm volatile("trap #15" : "+r"(d0) : : "d1", "d2", "a0", "a1", "memory");
  return d0;
}

/* ── Crash-safe mode: skip TVRAM output to avoid double fault ───────────── *
 *
 * When the crash handler runs after an IOCS _B_PUTC fault, calling _B_PUTC
 * again would cause a double bus/address error (CPU halt).  Setting this
 * flag makes uart_putc a no-op for TVRAM; klogf output goes only via the
 * serial mirror (_OUT232C).
 */
bool uart_tvram_inhibit;

/* ── VT100 → X68000 IOCS escape sequence converter ─────────────────────── *
 *
 * Intercepts ESC sequences before they reach IOCS _B_PUTC, parses them as
 * VT100 CSI, and converts supported sequences to IOCS calls:
 *
 *   ESC [ n A       → cursor up n        (_B_LOCATE)
 *   ESC [ n B       → cursor down n      (_B_LOCATE)
 *   ESC [ n C       → cursor forward n   (_B_LOCATE)
 *   ESC [ n D       → cursor back n      (_B_LOCATE)
 *   ESC [ row;col H → cursor position    (_B_LOCATE)
 *   ESC [ 2 J       → clear screen       (_B_CLR_ST)
 *   ESC [ 0 J       → clear to end       (_B_CLR_ED)
 *   ESC [ 0 K       → clear to EOL       (_B_CLR_AL)
 *   ESC [ ... m     → SGR colour         (_B_COLOR)
 *   ESC [ ? ...     → private modes (ignored)
 *
 * Cursor position is tracked in software to support relative movements.
 */

#define VT_MAX_PARAMS 8

/* Console text geometry, queried from IOCS _B_CONSOL at init.  Defaults to
 * the X68000 standard 96x31 text screen until the query updates them. */
static int vt_cols = 96;
static int vt_rows = 31;

enum { ST_NORMAL = 0, ST_ESC, ST_CSI };

static int vt_state;
static int vt_params[VT_MAX_PARAMS];
static int vt_nparams;
static int vt_private; /* '?' prefix seen */

/* Software cursor tracking */
static int cur_x, cur_y;

/* SGR (colour) state.  Mapped onto the X68000 default text palette
 * {0=black, 1=cyan, 2=yellow, 3=white} via IOCS _B_COLOR, with emphasis (+4)
 * for bold and reverse (+8) for inverse.  ANSI hues collapse to the nearest
 * of those four palette entries; the palette itself is left unchanged so
 * native Human68k programs still see Sharp's defaults. */
static int sgr_fg = 3; /* current text-palette entry (default: white) */
static int sgr_bold;
static int sgr_reverse;

static const unsigned char ansi_to_tpal[8] = {
    0, /* black             */
    2, /* red     -> yellow */
    1, /* green   -> cyan   */
    2, /* yellow            */
    1, /* blue    -> cyan   */
    3, /* magenta -> white  */
    1, /* cyan              */
    3, /* white             */
};

static void sgr_apply(void) {
  int code = sgr_fg & 3;
  if (sgr_bold) code += 4;
  if (sgr_reverse) code += 8;
  iocs_b_color(code);
}

static int param(int idx, int def) {
  if (idx < vt_nparams && vt_params[idx] > 0) return vt_params[idx];
  return def;
}

static void clamp_cursor(void) {
  if (cur_x < 0) cur_x = 0;
  if (cur_y < 0) cur_y = 0;
  if (cur_x >= vt_cols) cur_x = vt_cols - 1;
  if (cur_y >= vt_rows) cur_y = vt_rows - 1;
}

static void locate(int x, int y) {
  cur_x = x;
  cur_y = y;
  clamp_cursor();
  iocs_b_locate(cur_x, cur_y);
}

static void csi_dispatch(int final) {
  if (vt_private) return; /* ignore private modes (ESC[?...) */

  switch (final) {
    case 'A': /* Cursor Up */
      locate(cur_x, cur_y - param(0, 1));
      break;
    case 'B': /* Cursor Down */
      locate(cur_x, cur_y + param(0, 1));
      break;
    case 'C': /* Cursor Forward */
      locate(cur_x + param(0, 1), cur_y);
      break;
    case 'D': /* Cursor Back */
      locate(cur_x - param(0, 1), cur_y);
      break;
    case 'H':
    case 'f': /* Cursor Position (1-based) */
      locate(param(1, 1) - 1, param(0, 1) - 1);
      break;
    case 'J': /* Erase in Display */
      switch (param(0, 0)) {
        case 0:
          iocs_b_clr_ed();
          break; /* cursor to end */
        case 2:  /* entire screen */
          iocs_b_clr_st();
          cur_x = cur_y = 0;
          break;
      }
      break;
    case 'K':                                /* Erase in Line */
      if (param(0, 0) == 0) iocs_b_clr_al(); /* cursor to end of line */
      break;
    case 'm': /* SGR — map colour onto the X68000 text palette */
      if (vt_nparams == 0) {
        /* Bare ESC[m == ESC[0m == reset */
        sgr_fg = 3;
        sgr_bold = 0;
        sgr_reverse = 0;
      } else {
        for (int i = 0; i < vt_nparams; i++) {
          int p = vt_params[i];
          if (p == 0) {
            sgr_fg = 3;
            sgr_bold = 0;
            sgr_reverse = 0;
          } else if (p == 1) {
            sgr_bold = 1;
          } else if (p == 7) {
            sgr_reverse = 1;
          } else if (p == 22) {
            sgr_bold = 0;
          } else if (p == 27) {
            sgr_reverse = 0;
          } else if (p >= 30 && p <= 37) {
            sgr_fg = ansi_to_tpal[p - 30];
          } else if (p == 39) {
            sgr_fg = 3;
          } else if (p >= 90 && p <= 97) {
            sgr_fg = ansi_to_tpal[p - 90];
            sgr_bold = 1;
          }
          /* Background (40-47/49/100-107) is not represented: the IOCS
           * console has no per-character background; reverse (7) covers
           * inverse.  Unknown params are ignored. */
        }
      }
      sgr_apply();
      break;
    case 'r': /* DECSTBM — scroll region, ignore */
      break;
    default:
      break; /* unknown — silently ignore */
  }
}

/* Feed one character through the VT100 converter.
 * Returns 1 if the character was consumed (part of an escape sequence).
 * Returns 0 if the character should be passed to _B_PUTC normally. */
static int vt_feed(char c) {
  unsigned char ch = (unsigned char)c;

  switch (vt_state) {
    case ST_NORMAL:
      if (ch == 0x1Bu) {
        vt_state = ST_ESC;
        return 1; /* consumed */
      }
      /* Track cursor for printable characters */
      if (ch >= 0x20u && ch <= 0x7Eu) {
        cur_x++;
        if (cur_x >= vt_cols) {
          cur_x = 0;
          if (cur_y < vt_rows - 1) cur_y++;
        }
      } else if (ch == '\n') {
        if (cur_y < vt_rows - 1) cur_y++;
      } else if (ch == '\r') {
        cur_x = 0;
      } else if (ch == '\b') {
        if (cur_x > 0) cur_x--;
      } else if (ch == '\t') {
        cur_x = (cur_x + 8) & ~7;
        if (cur_x >= vt_cols) cur_x = vt_cols - 1;
      }
      return 0; /* pass through to _B_PUTC */

    case ST_ESC:
      if (ch == '[') {
        vt_state = ST_CSI;
        vt_nparams = 0;
        vt_private = 0;
        for (int i = 0; i < VT_MAX_PARAMS; i++) vt_params[i] = 0;
      } else if (ch == 'c') {
        /* RIS — full reset */
        vt_state = ST_NORMAL;
        iocs_b_clr_st();
        cur_x = cur_y = 0;
      } else {
        /* Unknown ESC+x — discard */
        vt_state = ST_NORMAL;
      }
      return 1;

    case ST_CSI:
      if (ch == '?') {
        vt_private = 1;
      } else if (ch >= '0' && ch <= '9') {
        if (vt_nparams == 0) vt_nparams = 1;
        vt_params[vt_nparams - 1] = vt_params[vt_nparams - 1] * 10 + (ch - '0');
      } else if (ch == ';') {
        if (vt_nparams < VT_MAX_PARAMS) vt_nparams++;
      } else if (ch >= 0x40u && ch <= 0x7Eu) {
        /* Final byte — dispatch */
        if (vt_nparams == 0 && ch != 'm') vt_nparams = 1;
        vt_state = ST_NORMAL;
        csi_dispatch(ch);
      } else {
        /* Unexpected byte — abort */
        vt_state = ST_NORMAL;
      }
      return 1;
  }

  vt_state = ST_NORMAL;
  return 0;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void uart_init(void) {
  /* IOCS owns the display and serial hardware.  Do not clear TVRAM here:
   * that would wipe stage1/2's "PiPA" + the kernel's "Po" banner.
   * cur_x/cur_y stay 0; the first VT100 sequence corrects them. */
  x68k_iocs_init();

  /* Query the real text geometry so the VT100 converter's wrap/clamp math
   * and the tty window size match the active TVRAM mode. */
  x68k_iocs_enter();
  x68k_iocs_irq_state_t irq = x68k_iocs_irq_begin();
  iocs_b_consol_query(&vt_cols, &vt_rows);
  x68k_iocs_irq_end(irq);
  x68k_iocs_exit();
  if (vt_cols < 20 || vt_cols > 128) vt_cols = 96;
  if (vt_rows < 8 || vt_rows > 64) vt_rows = 31;
}

/* Text console geometry for the tty backend's get_cols/get_rows hooks
 * (TIOCGWINSZ).  Reflects the IOCS _B_CONSOL query done in uart_init. */
int uart_get_cols(void) { return vt_cols; }
int uart_get_rows(void) { return vt_rows; }

int uart_putc(char c, void (*notify)(void)) {
  (void)notify; /* IOCS is synchronous — putc never fails */
  if (uart_tvram_inhibit) return 1;
  x68k_iocs_enter();
  x68k_iocs_irq_state_t irq = x68k_iocs_irq_begin();
  if (!vt_feed(c)) iocs_b_putc(c);
  x68k_iocs_irq_end(irq);
  x68k_iocs_exit();
  return 1;
}

int uart_getc(void) {
  x68k_iocs_enter();
  x68k_iocs_irq_state_t irq = x68k_iocs_irq_begin();
  int avail = iocs_b_keysns();
  if (!avail) {
    x68k_iocs_irq_end(irq);
    x68k_iocs_exit();
    return -1;
  }
  int c = iocs_b_getc();
  x68k_iocs_irq_end(irq);
  x68k_iocs_exit();
  if (c > 0x7F) return -1;
  return c;
}

int uart_rx_avail(void) {
  x68k_iocs_enter();
  x68k_iocs_irq_state_t irq = x68k_iocs_irq_begin();
  int r = iocs_b_keysns() ? 1 : 0;
  x68k_iocs_irq_end(irq);
  x68k_iocs_exit();
  return r;
}

/*
 * uart_rx_avail_idle — keyboard availability check for the idle poll
 *
 * Senses the IOCS key ring, not the MFP USART directly: the ROM keyboard
 * ISR drains the USART receive register into the ring within microseconds
 * of a byte arriving, so a process-context poll of RSR bit 7 (Buffer Full)
 * essentially never observes it.  Skips the poll (returns 0) when the IOCS
 * mutex is busy; the next idle pass retries.
 */
int uart_rx_avail_idle(void) {
  if (!x68k_iocs_try_enter()) return 0;
  x68k_iocs_irq_state_t irq = x68k_iocs_irq_begin();
  int r = iocs_b_keysns() ? 1 : 0;
  x68k_iocs_irq_end(irq);
  x68k_iocs_exit();
  return r;
}
