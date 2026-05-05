/*
 * sigaction.c — m68k user-space signal() wrapper.
 *
 * Builds a struct ppap_sigaction on the stack with sa_restorer set to
 * the _ppap_sigreturn_trampoline symbol (provided by this arch's
 * syscall.S), then forwards to SYS_RT_SIGACTION.  The C compiler
 * handles the GOT-relative reference to _ppap_sigreturn_trampoline
 * that -msep-data needs, so the runtime value carries the proper
 * load-relative address.
 *
 * Mirrors the ARM implementation; same sa_restorer model used by
 * glibc / musl on Linux.
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
