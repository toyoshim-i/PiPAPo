/*
 * PPAP config.h for Rogue 5.4.4
 *
 * This replaces the autoconf-generated config.h.
 * Only features available on PPAP are enabled.
 */

#ifndef PPAP_ROGUE_CONFIG_H
#define PPAP_ROGUE_CONFIG_H

/* System headers */
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_UNISTD_H 1
#define HAVE_STRING_H 1
#define HAVE_STDLIB_H 1
#define HAVE_LIMITS_H 1
#define HAVE_ARPA_INET_H 1
#define HAVE_TERMIOS_H 1
#define HAVE_CURSES_H 1

/* Functions */
#define HAVE_GETUID 1
#define HAVE_GETGID 1
/* HAVE_SETUID / HAVE_SETGID — not defined; PPAP returns ENOSYS.
 * md_normaluser() falls to if(0) no-ops, which is fine (single-user). */
#define HAVE_ERASECHAR 1
#define HAVE_KILLCHAR 1

/* Fork — PPAP has vfork semantics */
#define HAVE_WORKING_FORK 1

/* pwd.h — we provide a stub pwd.h that returns fixed values */
#define HAVE_PWD_H 1

/* getpass — musl provides it; avoids _getch() Windows reference */
#define HAVE_GETPASS 1

/* NOT defined — disables features we don't support:
 *
 * HAVE_GETPWUID  — md_getusername() guarded, falls back to env vars
 * HAVE_ALARM     — no SIGALRM support
 * HAVE_NLIST_H   — no /dev/kmem load average
 * HAVE_UTMP_H    — no user counting
 * HAVE_TERM_H    — no terminfo; clr_eol provided by our curses.h
 * HAVE_NCURSES_TERM_H — ditto
 * HAVE_GETLOADAVG     — no load average
 * HAVE_SETREUID       — no BSD-style uid switching
 * HAVE_SETREGID       — ditto
 * HAVE_SETRESUID      — ditto
 * HAVE_SETRESGID      — ditto
 * HAVE_ESCDELAY       — not applicable
 * CHECKTIME     — disables alarm()/SIGALRM load checking
 * SCOREFILE     — disables score file (initially)
 * MAXLOAD       — no load average checking
 * MAXUSERS      — no user count checking
 * MASTER        — disable wizard mode
 */

/* Package info */
#define PACKAGE "rogue"
#define PACKAGE_VERSION "5.4.4"
#define VERSION "5.4.4"

#endif /* PPAP_ROGUE_CONFIG_H */
