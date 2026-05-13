/*
 * cc_keymap.h — M5Stack CardComputer keymap data
 *
 * Static-const data tables consumed by cc_kbd.c.  Kept in a header
 * so the keymap stays close to its sole consumer; declared `static`
 * so multiple inclusion (which does not happen today) would still
 * be safe.  See docs/reference/cardcomputer.md §Keyboard for the
 * source-of-truth tables and the modifier-key positions.
 *
 * Layout: 4 logical rows × 14 logical columns = 56 keys.
 *   y=0: number row + Backspace
 *   y=1: Tab + QWERTY top row
 *   y=2: Fn + Shift + ASDF home row + Enter
 *   y=3: Ctrl + Opt + Alt + ZXCV bottom row + Space
 */

#ifndef PPAP_TARGET_XTENSA_CC_KERNEL_VFS_DRIVER_CC_KEYMAP_H
#define PPAP_TARGET_XTENSA_CC_KERNEL_VFS_DRIVER_CC_KEYMAP_H

#include <stdint.h>

/* ── Modifier key positions (logical y, x) ─────────────────────────────── */

#define CC_KEY_FN_Y    2u
#define CC_KEY_FN_X    0u
#define CC_KEY_SHIFT_Y 2u
#define CC_KEY_SHIFT_X 1u
#define CC_KEY_CTRL_Y  3u
#define CC_KEY_CTRL_X  0u
/* Opt at (3,1) and Alt at (3,2) are tracked-only; CC-5 does not act on them. */

/* ── Base + Shift table ────────────────────────────────────────────────── *
 *
 * Cell {base, shift}.  Modifier cells (Fn / Shift / Ctrl / Opt / Alt) are
 * encoded as 0 — the scan loop strips them from the candidate set before
 * resolving via this table, so the value is unreachable through normal
 * lookup.
 *
 * The table layout is preserved as a 4×14 grid for readability; the
 * `clang-format off` markers stop the formatter from breaking each cell
 * onto its own line.  The format check script does not scan target
 * overlays today, but the markers document intent for anyone running
 * clang-format manually.
 */

typedef struct {
  uint8_t base;
  uint8_t shift;
} cc_keymap_cell_t;

#define _CC_BS  0x08u  /* Backspace */
#define _CC_TAB 0x09u  /* Tab       */
#define _CC_ENT 0x0Du  /* Enter / CR (line discipline maps to LF if needed) */
#define _CC_SP  0x20u  /* Space     */
#define _CC_M   0u     /* Modifier — handled out-of-band */

/* clang-format off */
static const cc_keymap_cell_t cc_keymap[4][14] = {
    /* y=0: number row + Backspace */
    {{'`',  '~'}, {'1', '!'}, {'2',  '@'}, {'3',  '#'},
     {'4',  '$'}, {'5', '%'}, {'6',  '^'}, {'7',  '&'},
     {'8',  '*'}, {'9', '('}, {'0',  ')'}, {'-',  '_'},
     {'=',  '+'}, {_CC_BS, _CC_BS}},

    /* y=1: Tab + QWERTY top row */
    {{_CC_TAB, _CC_TAB}, {'q', 'Q'}, {'w', 'W'}, {'e', 'E'},
     {'r',     'R'},     {'t', 'T'}, {'y', 'Y'}, {'u', 'U'},
     {'i',     'I'},     {'o', 'O'}, {'p', 'P'}, {'[', '{'},
     {']',     '}'},     {'\\', '|'}},

    /* y=2: Fn + Shift + ASDF home row + Enter */
    {{_CC_M, _CC_M},  {_CC_M, _CC_M}, {'a', 'A'}, {'s', 'S'},
     {'d',   'D'},    {'f', 'F'},     {'g', 'G'}, {'h', 'H'},
     {'j',   'J'},    {'k', 'K'},     {'l', 'L'}, {';', ':'},
     {'\'',  '"'},    {_CC_ENT, _CC_ENT}},

    /* y=3: Ctrl + Opt + Alt + ZXCV bottom row + Space */
    {{_CC_M, _CC_M}, {_CC_M, _CC_M}, {_CC_M, _CC_M}, {'z', 'Z'},
     {'x',   'X'},   {'c', 'C'},     {'v', 'V'},     {'b', 'B'},
     {'n',   'N'},   {'m', 'M'},     {',', '<'},     {'.', '>'},
     {'/',   '?'},   {_CC_SP, _CC_SP}},
};
/* clang-format on */

/* ── PPAP Fn-layer (silkscreen-aligned) ────────────────────────────────── *
 *
 * Returns a NUL-terminated VT100 escape string for the (y, x) key when
 * Fn is held, or NULL if no Fn-mapping is defined (in which case the
 * caller falls back to the base/shift table).  See
 * docs/proposals/cardcomputer_port.md §5.1.
 *
 * F-key sequences use the SS3-prefix form (\033O[P..Y]) for F1–F10,
 * which most VT100-compatible terminals accept and which produces
 * compact 3-byte sequences.
 */

typedef struct {
  uint8_t y;
  uint8_t x;
  const char *seq;
} cc_fn_entry_t;

/* clang-format off */
static const cc_fn_entry_t cc_fn_layer[] = {
    /* Top-left ` → Esc */
    {0,  0, "\x1b"},
    /* Number row 1..0 → F1..F10 */
    {0,  1, "\x1bOP"}, {0,  2, "\x1bOQ"}, {0,  3, "\x1bOR"},
    {0,  4, "\x1bOS"}, {0,  5, "\x1bOT"}, {0,  6, "\x1bOU"},
    {0,  7, "\x1bOV"}, {0,  8, "\x1bOW"}, {0,  9, "\x1bOX"},
    {0, 10, "\x1bOY"},
    /* Backspace → Delete */
    {0, 13, "\x1b[3~"},
    /* Home row ; → ↑ (silkscreen places ; above , . /) */
    {2, 11, "\x1b[A"},
    /* Bottom row , . / → ← ↓ → */
    {3, 10, "\x1b[D"}, {3, 11, "\x1b[B"}, {3, 12, "\x1b[C"},
};
/* clang-format on */

#define CC_FN_LAYER_COUNT (sizeof(cc_fn_layer) / sizeof(cc_fn_layer[0]))

static inline const char *cc_fn_lookup(int y, int x) {
  for (unsigned i = 0; i < CC_FN_LAYER_COUNT; i++) {
    if (cc_fn_layer[i].y == y && cc_fn_layer[i].x == x)
      return cc_fn_layer[i].seq;
  }
  return (const char *)0;
}

#endif /* PPAP_TARGET_XTENSA_CC_KERNEL_VFS_DRIVER_CC_KEYMAP_H */
