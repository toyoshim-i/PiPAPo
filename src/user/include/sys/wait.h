/*
 * <sys/wait.h> — waitpid() flags and status macros.
 *
 * Forwards the macros from src/common/wait.h and declares the
 * POSIX wait() / waitpid() entry points.
 */

#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <sys/types.h>

#include "common/wait.h"

pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait(int *status);

#endif /* _SYS_WAIT_H */
