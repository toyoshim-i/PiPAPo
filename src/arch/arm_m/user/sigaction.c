/*
 * sigaction.c — ARM user-space sigaction() wrapper.
 *
 * Builds a struct sigaction on the stack with sa_restorer set to the
 * _ppap_sigreturn_trampoline symbol (provided by this arch's
 * syscall.S), then forwards to SYS_RT_SIGACTION.  The C compiler
 * handles PIC correctly for &_ppap_sigreturn_trampoline — taking the
 * address goes through the GOT, so the runtime value carries the
 * Thumb bit and the relocated text base.
 *
 * Modeled on the glibc/musl convention: libc owns the sigreturn
 * trampoline; the kernel just takes its address via sa_restorer.
 */

#include "user/syscall.h"

struct ppap_sigaction {
  void (*sa_handler)(int);
  unsigned long sa_flags;
  void (*sa_restorer)(void);
  unsigned long sa_mask[2];
};

extern void _ppap_sigreturn_trampoline(void);
extern int rt_sigaction(int sig, const struct ppap_sigaction *act,
                        struct ppap_sigaction *oact, int sigsetsize);

int sigaction(int sig, void *handler, void *old_handler) {
  struct ppap_sigaction act;
  (void)old_handler; /* callers that need it should use rt_sigaction() */

  act.sa_handler = (void (*)(int))handler;
  act.sa_flags = 0;
  act.sa_restorer = _ppap_sigreturn_trampoline;
  act.sa_mask[0] = 0;
  act.sa_mask[1] = 0;
  return rt_sigaction(sig, &act, (struct ppap_sigaction *)0, 8);
}
