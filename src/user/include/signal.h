/*
 * <signal.h> — signal numbers, signal() handler installer.
 *
 * Numeric SIG* constants come from src/common/signal.h (kernel/user
 * shared).  The signal() implementation lives in
 * src/arch/<arch>/user/sigaction.c — only arches that ship a
 * _ppap_sigreturn_trampoline (ARM, m68k, RISC-V) provide it; on the
 * other targets, signal() is unavailable.
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

#endif /* _SIGNAL_H */
