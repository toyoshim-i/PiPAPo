/*
 * pi_ui.c — Screen refresh for PiPAPo Editor
 *
 * Renders: menu bar, content area with line numbers, tilde lines,
 * dropdown overlay, status bar, and hint bar.
 *
 * Stub — full implementation in E-4.
 */

#include "pi.h"

void ui_set_status(const char *msg) {
  int i = 0;
  while (msg[i] && i < (int)sizeof(E.status) - 1) {
    E.status[i] = msg[i];
    i++;
  }
  E.status[i] = '\0';
}

void ui_refresh(void) {
  /* Stub — will be implemented in E-4 */
}
