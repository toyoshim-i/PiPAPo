/*
 * signal.c — POSIX raise() (arch-neutral).
 *
 * signal() lives in the shared src/user/lib/sigaction.c on arches whose
 * cmake config sets PPAP_USER_HAS_SIGRETURN_TRAMPOLINE.  raise() itself
 * is portable: send a signal to the current process.
 */

#include "syscall.h"

#include <signal.h>

int raise(int sig) { return kill(getpid(), sig); }
