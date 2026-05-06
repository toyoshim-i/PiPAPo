/*
 * signal.c — POSIX raise() (arch-neutral).
 *
 * signal() itself is arch-specific (lives in src/arch/<arch>/user/
 * sigaction.c, since it needs the per-arch sigreturn trampoline).
 * raise() is portable: send a signal to the current process.
 */

#include "syscall.h"

#include <signal.h>

int raise(int sig) { return kill(getpid(), sig); }
