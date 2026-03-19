/*
 * push.h — PiPAPo μShell shared definitions
 *
 * Shared between push.c (core) and push_line.c (line editor).
 */

#ifndef PPAP_USER_PUSH_H
#define PPAP_USER_PUSH_H

/* ── Buffer limits ───────────────────────────────────────────────────── */

#define PUSH_LINE_MAX    256   /* max input line length               */
#define PUSH_HISTORY_MAX 32    /* history ring buffer depth           */
#define PUSH_TOK_BUF     384   /* expanded token buffer               */
#define PUSH_TOKEN_MAX   64    /* max tokens per line                 */
#define PATH_BUF         128   /* max path length                     */

/* ── Line editor interface (push_line.c) ─────────────────────────────── */

/*
 * push_readline() — read a line with editing and history support.
 *
 * Displays prompt, reads user input with VT100 line editing,
 * and returns the completed line in buf (NUL-terminated).
 *
 * If prompt is NULL, renders PS1 from environment.
 *
 * Returns line length, or -1 on EOF (Ctrl-D on empty line).
 * Returns 0 for empty line (e.g., after Ctrl-C).
 */
int push_readline(const char *prompt, char *buf, int size);

/*
 * push_history_add() — add a line to the history ring buffer.
 * Duplicate consecutive entries are suppressed.
 */
void push_history_add(const char *line);

/*
 * push_history_list() — print all history entries to fd.
 */
void push_history_list(int fd);

#endif /* PPAP_USER_PUSH_H */
