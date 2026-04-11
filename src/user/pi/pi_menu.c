/*
 * pi_menu.c — Menu data tables and dispatch for PiPAPo Editor
 *
 * All menu items map to vi-compatible shortcuts shown alongside.
 * The dispatch function triggers the corresponding editor action.
 */

#include "pi.h"

/* ── Menu definitions ──────────────────────────────────────────────────── */

const menu_cat_t pi_menus[MENU_CAT_COUNT] = {
    {
        .label = "File",
        .col = 1,
        .count = 7,
        .items =
            {
                {"New", ":e!", 0},
                {"Open", ":e", 0},
                {"Save", ":w", 0},
                {"Save As", ":w f", 0},
                {"Quit", ":q", 1},
                {"Force Quit", ":q!", 0},
                {"Save+Quit", ":wq", 0},
            },
    },
    {
        .label = "Edit",
        .col = 7,
        .count = 8,
        .items =
            {
                {"Insert", "i", 0},
                {"Append", "a", 0},
                {"Open Below", "o", 0},
                {"Open Above", "O", 0},
                {"Delete Char", "x", 1},
                {"Delete Line", "dd", 0},
                {"Undo", "u", 1},
            },
    },
    {
        .label = "Search",
        .col = 13,
        .count = 3,
        .items =
            {
                {"Find", "/", 0},
                {"Next", "n", 0},
                {"Prev", "N", 0},
            },
    },
    {
        .label = "Go",
        .col = 21,
        .count = 3,
        .items =
            {
                {"Top", "gg", 0},
                {"Bottom", "G", 0},
                {"Line...", ":N", 0},
            },
    },
    {
        .label = "Help",
        .col = 25,
        .count = 2,
        .items =
            {
                {"Keys", "F1", 0},
                {"About", "", 0},
            },
    },
};

/* ── Forward declarations for actions ───────────────��──────────────────── */

/* These will be implemented in later steps (E-5..E-9).
 * For now, dispatch sets a status message and returns. */

int menu_dispatch(int cat, int item) {
  /* Bounds check */
  if (cat < 0 || cat >= MENU_CAT_COUNT)
    return 0;
  if (item < 0 || item >= pi_menus[cat].count)
    return 0;

  /* File menu */
  if (cat == 0) {
    switch (item) {
    case 0: /* New */
      ui_set_status("New: not yet implemented");
      break;
    case 1: /* Open */
      ui_set_status("Open: not yet implemented");
      break;
    case 2: /* Save */
      ui_set_status("Save: not yet implemented");
      break;
    case 3: /* Save As */
      ui_set_status("Save As: not yet implemented");
      break;
    case 4: /* Quit */
      if (E.dirty) {
        ui_set_status("Unsaved changes — use :q! to force quit");
        return 0;
      }
      return 1;
    case 5: /* Force Quit */
      return 1;
    case 6: /* Save+Quit */
      ui_set_status("Save+Quit: not yet implemented");
      break;
    }
    return 0;
  }

  /* Edit menu */
  if (cat == 1) {
    switch (item) {
    case 0: /* Insert */
      E.mode = MODE_INSERT;
      break;
    case 1: /* Append */
      E.mode = MODE_INSERT;
      /* Move cursor forward one (append) */
      if (E.cx < gap_row_len(&E.buf, E.cy))
        E.cx++;
      gap_move(&E.buf, gap_pos_from_rc(&E.buf, E.cy, E.cx));
      break;
    case 2: /* Open Below */
      E.mode = MODE_INSERT;
      {
        int eol = gap_pos_from_rc(&E.buf, E.cy, gap_row_len(&E.buf, E.cy));
        gap_move(&E.buf, eol);
        gap_insert(&E.buf, '\n');
        E.cy++;
        E.cx = 0;
      }
      break;
    case 3: /* Open Above */
      E.mode = MODE_INSERT;
      {
        int sol = gap_pos_from_rc(&E.buf, E.cy, 0);
        gap_move(&E.buf, sol);
        gap_insert(&E.buf, '\n');
        /* Cursor stays on the new blank line (which is now E.cy) */
        gap_move(&E.buf, sol);
        E.cx = 0;
      }
      break;
    case 4: /* Delete Char */
      gap_move(&E.buf, gap_pos_from_rc(&E.buf, E.cy, E.cx));
      gap_delete_fwd(&E.buf);
      E.dirty = 1;
      break;
    case 5: /* Delete Line */
      gap_move(&E.buf, gap_pos_from_rc(&E.buf, E.cy, 0));
      gap_delete_line(&E.buf);
      E.cx = 0;
      E.dirty = 1;
      break;
    case 6: /* Undo */
      ui_set_status("Undo: not yet implemented");
      break;
    }
    return 0;
  }

  /* Search menu */
  if (cat == 2) {
    ui_set_status("Search: not yet implemented");
    return 0;
  }

  /* Go menu */
  if (cat == 3) {
    switch (item) {
    case 0: /* Top */
      E.cy = 0;
      E.cx = 0;
      break;
    case 1: /* Bottom */
      E.cy = gap_line_count(&E.buf) - 1;
      E.cx = 0;
      break;
    case 2: /* Line... */
      E.mode = MODE_COMMAND;
      E.cmd[0] = '\0';
      E.cmd_len = 0;
      break;
    }
    return 0;
  }

  /* Help menu */
  if (cat == 4) {
    switch (item) {
    case 0: /* Keys */
      ui_set_status("F1 help: not yet implemented");
      break;
    case 1: /* About */
      ui_set_status("pi - PiPAPo Editor");
      break;
    }
    return 0;
  }

  return 0;
}
