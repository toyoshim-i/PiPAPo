/*
 * push.h — PiPAPo μShell shared definitions
 *
 * Shared between push.c (core) and push_line.c (line editor).
 */

#ifndef PPAP_USER_PUSH_H
#define PPAP_USER_PUSH_H

/* ── Buffer limits ───────────────────────────────────────────────────── */

#define PUSH_LINE_MAX 256   /* max input line length               */
#define PUSH_HISTORY_MAX 32 /* history ring buffer depth           */
#define PUSH_HIST_POOL 1024 /* packed history char pool             */
#define PUSH_TOK_BUF 1024   /* expanded token buffer (tokens + globs) */
#define PUSH_TOKEN_MAX 64   /* max tokens per line                 */
#define PATH_BUF 128        /* max path length                     */

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

/*
 * push_env_get() — look up a shell variable by name.
 * Returns the value string (after '='), or NULL if not set.
 * Used by push_line.c for PATH-based command completion.
 */
const char *push_env_get(const char *key);

/*
 * push_is_builtin() — check if cmd is a shell builtin.
 * Used by push_line.c for command completion.
 */
int push_is_builtin(const char *cmd);

/*
 * ext_allowed() — check if extension is in PATHEXT.
 * ext is the extension without dot (e.g. "COM").
 * pathext is the PATHEXT value (e.g. ".COM;.OBJ;.X;.R"), or NULL (allow all).
 */
int ext_allowed(const char *ext, const char *pathext);

#endif /* PPAP_USER_PUSH_H */
