/*
 * pi.c — PiPAPo Editor main
 *
 * Entry point and main loop.  Full implementation in E-5.
 */

#include "pi.h"

/* Global editor state */
editor_t E;

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  /* Initialize terminal */
  term_raw();
  term_get_winsize(&E.rows, &E.cols);

  /* Initialize gap buffer */
  gap_init(&E.buf, 4096);

  /* Start in normal mode */
  E.mode = MODE_NORMAL;
  E.menu_cat = 0;
  E.menu_item = 0;
  ui_set_status("pi - PiPAPo Editor | Esc: menu");

  /* Main loop — placeholder, just handles quit */
  while (!E.quit) {
    ui_refresh();
    int key = pi_read_key();
    if (key == KEY_NONE)
      continue;

    if (key == ':') {
      /* Quick :q for testing */
      int k2 = pi_read_key();
      if (k2 == 'q') {
        int k3 = pi_read_key();
        if (k3 == KEY_ENTER || k3 == KEY_NONE)
          E.quit = 1;
      }
    }
  }

  /* Cleanup */
  term_clear();
  term_cursor_to(0, 0);
  term_attr_reset();
  term_cursor_show();
  term_restore();

  return 0;
}
