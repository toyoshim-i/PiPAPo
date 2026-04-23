/*
 * pile.h — PiPAPo two-pane filer, shared types and declarations
 *
 * Design: docs/proposals/pile.md
 */

#ifndef PPAP_USER_PILE_H
#define PPAP_USER_PILE_H

#include "lib/uclib.h"

#include "common/termios.h"

/* ── Limits ────────────────────────────────────────────────────────────── */

#define PILE_MAX_ENTRIES 256
#define PILE_PATH_MAX    128

/* Layout thresholds, see docs/proposals/pile.md "Layout adaptation". */
#define PILE_MIN_COLS      40  /* below this, refuse to run */
#define PILE_TWOPANE_COLS  70  /* ≥ this, use two panes */

/* ── Key codes ─────────────────────────────────────────────────────────── */

enum {
  PKEY_NONE = 0,
  PKEY_ENTER = '\r',
  PKEY_TAB = '\t',
  PKEY_BS = 8,
  PKEY_BACKSPACE = 127,
  PKEY_ESC = 0x100,
  PKEY_UP,
  PKEY_DOWN,
  PKEY_LEFT,
  PKEY_RIGHT,
  PKEY_HOME,
  PKEY_END,
  PKEY_DELETE,
  PKEY_PGUP,
  PKEY_PGDN,
  PKEY_F1,
  PKEY_F3,
  PKEY_F4,
  PKEY_F5,
  PKEY_F6,
  PKEY_F7,
  PKEY_F8,
  PKEY_F9,
  PKEY_F10,
};

/* ── Pane model ────────────────────────────────────────────────────────── */

/* Single directory entry.  76 bytes.  Fixed-size name keeps the entry
 * array flat; names longer than 63 chars (the VFS limit) cannot occur. */
typedef struct {
  char name[64];       /* NUL-terminated, up to PPAP_NAME_MAX + 1 */
  uint32_t size;
  uint32_t mtime;
  uint16_t mode;
  uint8_t d_type;
  uint8_t flags;       /* bit0: marked (P2+), bit1: stat failed */
} pile_entry_t;

#define PILE_EFLAG_MARKED    0x01
#define PILE_EFLAG_STATFAIL  0x02

typedef struct {
  char path[PILE_PATH_MAX];
  pile_entry_t entries[PILE_MAX_ENTRIES];
  int count;
  int cursor;    /* index into entries[] */
  int scroll;    /* first visible row */
  int truncated; /* 1 if more than PILE_MAX_ENTRIES entries in dir */
} pile_pane_t;

/* ── Global state ──────────────────────────────────────────────────────── */

/* Layout classification, set once at startup from pile_cols.  SIGWINCH
 * re-detection is wired in P5. */
enum {
  PILE_LAYOUT_SINGLE,
  PILE_LAYOUT_TWO,
};

extern pile_pane_t pile_pane_a;
extern pile_pane_t pile_pane_b;
extern pile_pane_t *pile_active;  /* &pile_pane_a or &pile_pane_b */

extern int pile_rows;     /* terminal rows from TIOCGWINSZ */
extern int pile_cols;     /* terminal cols */
extern int pile_layout;   /* PILE_LAYOUT_SINGLE | PILE_LAYOUT_TWO */
extern int pile_quit;
extern int pile_use_color;

/* ── Terminal (pile.c) ─────────────────────────────────────────────────── */

void pile_term_raw(void);
void pile_term_restore(void);
int pile_read_key(void);

/* ── Pane operations (pile_pane.c) ─────────────────────────────────────── */

/* Load pane->entries from pane->path.  Resets cursor/scroll.  Returns 0
 * on success, -1 if the directory cannot be opened (leaves pane empty). */
int pile_pane_load(pile_pane_t *pane);

/* Move cursor; clamps to [0, count-1] and adjusts scroll. */
void pile_pane_move(pile_pane_t *pane, int delta, int visible_rows);
void pile_pane_home(pile_pane_t *pane);
void pile_pane_end(pile_pane_t *pane, int visible_rows);

/* Handle ENTER on the current entry.  Returns 1 if the pane changed
 * (caller should redraw).  P1 handles directories only; file actions
 * land in P3. */
int pile_pane_enter(pile_pane_t *pane);

/* Change into the parent directory (same effect as ENTER on ".."). */
int pile_pane_parent(pile_pane_t *pane);

/* Marking.  Mark state lives in pile_entry_t.flags under PILE_EFLAG_MARKED
 * and is cleared implicitly by pile_pane_load() (fresh entry array). */
void pile_pane_mark_toggle(pile_pane_t *pane, int vrows);
void pile_pane_mark_invert(pile_pane_t *pane);
int pile_pane_mark_glob(pile_pane_t *pane, const char *pattern, int mark);
int pile_pane_sel_count(const pile_pane_t *pane);

/* ── Drawing (pile_draw.c) ─────────────────────────────────────────────── */

void pile_draw_all(void);
void pile_draw_clear(void);

/* Number of entry rows visible in a pane given the current terminal
 * height and chrome layout.  Shared between drawing and key handling
 * (PgUp / PgDn step size, scroll clamping). */
int pile_draw_visible_rows(void);

#endif /* PPAP_USER_PILE_H */
