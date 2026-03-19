/*
 * push.h — PiPAPo μShell shared definitions
 *
 * Shared between push.c (core) and push_line.c (line editor, Phase 3).
 * Phase 1 uses push.c only; push_line.c is added in Phase 3.
 */

#ifndef PPAP_USER_PUSH_H
#define PPAP_USER_PUSH_H

/* ── Buffer limits ───────────────────────────────────────────────────── */

#define PUSH_LINE_MAX    256   /* max input line length               */
#define PUSH_HISTORY_MAX 32    /* history ring buffer depth           */
#define PUSH_TOK_BUF     384   /* expanded token buffer               */
#define PUSH_TOKEN_MAX   64    /* max tokens per line                 */

/* ── Line editor interface (Phase 3) ─────────────────────────────────── */

/*
 * push_readline() — read a line with editing and history support.
 *
 * Displays prompt, reads user input with VT100 line editing,
 * and returns the completed line in buf (NUL-terminated).
 *
 * Returns line length, or -1 on EOF (Ctrl-D on empty line).
 *
 * Phase 1 stub: not implemented; push.c uses raw read_line() instead.
 */
/* int push_readline(const char *prompt, char *buf, int size); */

/*
 * push_history_add() — add a line to the history ring buffer.
 * Duplicate consecutive entries are suppressed.
 */
/* void push_history_add(const char *line); */

/*
 * push_history_list() — print all history entries to fd.
 */
/* void push_history_list(int fd); */

#endif /* PPAP_USER_PUSH_H */
