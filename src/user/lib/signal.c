/*
 * signal.c — POSIX raise() (arch-neutral).
 *
 * signal() itself lives in the shared src/user/lib/sigaction.c (built
 * as a regular libc unit by cmake/user.cmake).  raise() is portable:
 * send a signal to the current process.
 */

#include "syscall.h"

#include <signal.h>

int raise(int sig) { return kill(getpid(), sig); }
