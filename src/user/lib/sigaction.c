/*
 * sigaction.c — POSIX signal() shim, shared across arches that follow
 * the sa_restorer model.
 *
 * Builds a struct ppap_sigaction on the stack with sa_restorer set to
 * the per-arch _ppap_sigreturn_trampoline symbol (provided by each
 * arch's user/syscall.S) and forwards to SYS_RT_SIGACTION.  Same
 * convention as a Linux libc: libc owns the sigreturn trampoline,
 * the kernel just records its address via sa_restorer.
 *
 * Compiled into PPAP libc as a regular libc unit by cmake/user.cmake.
 * Every arch that goes through user.cmake must therefore provide
 * _ppap_sigreturn_trampoline in its user/syscall.S; an arch that
 * supplies its own signal() (e.g. i16's real-mode-friendly asm version)
 * does not go through user.cmake.
 */

#include <signal.h>

#include "user/syscall.h"

sighandler_t signal(int sig, sighandler_t handler) {
  struct ppap_sigaction act, old;
  act.sa_handler = handler;
  act.sa_flags = 0;
  act.sa_restorer = _ppap_sigreturn_trampoline;
  act.sa_mask[0] = 0;
  act.sa_mask[1] = 0;
  if (rt_sigaction(sig, &act, &old, 8) < 0) return SIG_ERR;
  return old.sa_handler ? old.sa_handler : SIG_DFL;
}
