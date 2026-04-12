/*
 * pi.h — PiPAPo Editor shared types and declarations
 *
 * Tiny vi-like editor with menu-driven UI for discoverability.
 */

#ifndef PPAP_USER_PI_H
#define PPAP_USER_PI_H

#include "lib/uclib.h"
#include "common/termios.h"

/* ── Modes ─────────────────────────────────────────────────────────────── */

enum {
  MODE_NORMAL,
  MODE_INSERT,
  MODE_MENU,
  MODE_COMMAND,
  MODE_HELP,
};

/* ── Key codes (returned by pi_read_key) ──────────────────────────────── */

enum {
  KEY_NONE = 0,
  KEY_ENTER = '\r',
  KEY_TAB = '\t',
  KEY_BACKSPACE = 127,
  KEY_BS = 8,
  KEY_ESC = 0x100, /* distinct from raw 0x1b */
  KEY_UP,
  KEY_DOWN,
  KEY_LEFT,
  KEY_RIGHT,
  KEY_HOME,
  KEY_END,
  KEY_DELETE,
  KEY_PGUP,
  KEY_PGDN,
  KEY_F1,
  KEY_F10,
};

/* ── Gap buffer ────────────────────────────────────────────────────────── */

typedef struct {
  char *data;
  int cap;
  int gap_start; /* logical cursor position */
  int gap_end;   /* gap_start + gap_size */
} gap_buf_t;

void gap_init(gap_buf_t *g, int initial_cap);
void gap_insert(gap_buf_t *g, char c);
void gap_delete(gap_buf_t *g);       /* delete char before gap (backspace) */
void gap_delete_fwd(gap_buf_t *g);   /* delete char after gap (delete key) */
void gap_delete_line(gap_buf_t *g);  /* delete current line (dd) */
void gap_move(gap_buf_t *g, int pos);
int gap_length(gap_buf_t *g);        /* text length (excluding gap) */
int gap_line_count(gap_buf_t *g);
int gap_char_at(gap_buf_t *g, int pos);

/* Get row content: copies row `row` (0-based) into buf, returns length.
 * Also sets *row_start to the absolute text position of the row start. */
int gap_get_row(gap_buf_t *g, int row, char *buf, int bufsize, int *row_start);

/* Convert (row, col) to absolute text position */
int gap_pos_from_rc(gap_buf_t *g, int row, int col);

/* Convert absolute position to (row, col) */
void gap_rc_from_pos(gap_buf_t *g, int pos, int *row, int *col);

/* Get row length (chars, not including newline) */
int gap_row_len(gap_buf_t *g, int row);

/* Single-level undo: snapshot and restore full buffer text */
void gap_snapshot(gap_buf_t *g, char **snap, int *snap_len);
void gap_restore(gap_buf_t *g, const char *snap, int snap_len);

/* Search: return position of match, or -1 if not found */
int gap_search_fwd(gap_buf_t *g, const char *needle, int needle_len,
                   int start_pos);
int gap_search_bwd(gap_buf_t *g, const char *needle, int needle_len,
                   int start_pos);

/* ── Menu ──────────────────────────────────────────────────────────────── */

#define MENU_CAT_COUNT 5
#define MENU_MAX_ITEMS 8

typedef struct {
  const char *label;
  const char *shortcut;
  int separator; /* 1 = draw separator line before this item */
} menu_item_t;

typedef struct {
  const char *label;
  int col;          /* column offset on menu bar */
  int count;
  menu_item_t items[MENU_MAX_ITEMS];
} menu_cat_t;

extern const menu_cat_t pi_menus[MENU_CAT_COUNT];

/* Execute menu action; returns 1 if the editor should quit */
int menu_dispatch(int cat, int item);

/* ── Editor state ──────────────────────────────────────────────────────── */

typedef struct {
  gap_buf_t buf;
  char filename[256];
  int cx, cy;        /* cursor col, row in file */
  int scroll_row;    /* first visible row */
  int rows, cols;    /* terminal size */
  int dirty;
  int mode;
  int quit;          /* set to 1 to exit main loop */
  /* menu */
  int menu_cat;
  int menu_item;
  /* command line */
  char cmd[128];
  int cmd_len;
  /* status message */
  char status[128];
  /* search */
  char search[64];
  int search_len;
  /* single-level undo */
  char *undo_snap;
  int undo_len;
  int undo_cx, undo_cy;
  /* pending 'g' for gg */
  int pending_g;
  /* pending 'd' for dd */
  int pending_d;
} editor_t;

extern editor_t E;

/* ── Terminal (pi_term.c) ──────────────────────────────────────────────── */

void term_raw(void);
void term_restore(void);
void term_get_winsize(int *rows, int *cols);
int pi_read_key(void);

/* ANSI output helpers */
void term_clear(void);
void term_cursor_to(int row, int col); /* 0-based */
void term_cursor_hide(void);
void term_cursor_show(void);
void term_attr_reset(void);
void term_attr_reverse(void);
void term_attr_dim(void);
void term_attr_bold(void);
void term_attr_fg(int color); /* 0-7 ANSI colors */
void term_attr_bg(int color);

/* ANSI color constants */
enum {
  COL_BLACK,
  COL_RED,
  COL_GREEN,
  COL_YELLOW,
  COL_BLUE,
  COL_MAGENTA,
  COL_CYAN,
  COL_WHITE,
};

/* ── File I/O (pi.c) ───────────────────────────────────────────────────── */

int save_file(const char *path);
void reset_buffer(void);
void load_file(const char *path);

/* ── Search (pi.c) ─────────────────────────────────────────────────────── */

void search_next(void);
void search_prev(void);

/* ── UI (pi_ui.c) ──────────────────────────────────────────────────────── */

void ui_refresh(void);
void ui_set_status(const char *msg);

#endif /* PPAP_USER_PI_H */
