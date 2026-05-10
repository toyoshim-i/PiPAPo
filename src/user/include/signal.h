/*
 * <signal.h> — signal numbers, signal() handler installer.
 *
 * Numeric SIG* constants come from src/common/signal.h (kernel/user
 * shared).  signal() is provided by the shared shim in
 * src/user/lib/sigaction.c for every arch that goes through
 * cmake/user.cmake; i16 supplies its own real-mode-friendly asm
 * signal() and does not go through user.cmake.
 *
 * POSIX-style sigaction() taking struct sigaction is not yet provided.
 */

#ifndef _SIGNAL_H
#define _SIGNAL_H

#include "common/signal.h"

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

sighandler_t signal(int sig, sighandler_t handler);
int raise(int sig);

#endif /* _SIGNAL_H */
