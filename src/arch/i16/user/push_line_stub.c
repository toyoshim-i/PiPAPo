/*
 * push_line_stub.c -- Minimal ia16 fallback for push line editing
 *
 * Provides a simple readline implementation without VT100 editing so
 * the shell can run on i16 before the full interactive line editor and
 * supporting syscall wrappers are wired up.
 */

#include "push.h"

#include "syscall.h"

static int stub_strlen(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

int push_readline(const char *prompt, char *buf, int size) {
  int len = 0;

  if (prompt) write(2, prompt, stub_strlen(prompt));

  while (len < size - 1) {
    char c;
    ssize_t n = read(0, &c, 1);

    if (n <= 0) {
      if (len == 0) return -1;
      break;
    }
    if (c == '\r') continue;
    if (c == '\n') break;
    buf[len++] = c;
  }
  buf[len] = '\0';
  return len;
}

void push_history_add(const char *line) {
  (void)line;
}

void push_history_list(int fd) {
  (void)fd;
}
