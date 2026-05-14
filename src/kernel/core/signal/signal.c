/*
 * signal.c — Architecture-independent signal core
 *
 *   signal_default_action(sig, regs)    — SIGCHLD-ignore / subsys hook /
 *                                         sys_exit dispatch
 *   signal_pop_pending(&sig, regs)      — preamble shared by every arch's
 *                                         signal_check: state guard, pop
 *                                         lowest pending, apply SIG_IGN
 *                                         and SIG_DFL dispositions
 *   signal_check_kernel()               — emulator-backed-task variant
 *                                         (SIG_IGN / SIG_DFL only)
 *   sys_kill(pid, sig)                  — post a pending signal
 *   sys_sigaction(sig, hdl, old)        — legacy 3-arg, kernel-internal
 *                                         (only ktest still calls it)
 *   sys_rt_sigaction(sig, act, oact, …) — musl-compatible install/query
 *   sys_rt_sigprocmask(how, set, oset, …)
 *
 * Per-arch signal_check and sys_rt_sigreturn live in
 * src/arch/<arch>/kernel/core/signal/signal_check.c — they build the
 * arch-specific delivery frame and unwind it.  All arches share the
 * sa_restorer model: user-space libc/crt supplies the sigreturn
 * trampoline whose address is recorded per handler by rt_sigaction.
 */

#include "kernel/core/signal/signal.h"

#include <stddef.h>
#include <stdint.h>

#include "common/errno.h"
#include "kernel/common/bitops.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/subsys/subsys.h"
#include "kernel/core/syscall/syscall.h"

struct kernel_sigaction {
  uint32_t handler;
  uint32_t flags;
  uint32_t restorer;
  uint32_t mask[2];
};

int signal_default_action(int sig, uint32_t *regs) {
  if (sig == SIGCHLD) return 1;

  {
    const subsys_ops_t *ops =
        current->subsys < SUBSYS_MAX ? subsys_ops_table[current->subsys] : 0;
    if (ops && ops->on_signal && ops->on_signal(current, sig, regs)) return 1;
  }

  sys_exit(128 + sig);
  return 1;
}

int signal_pop_pending(int *sig_out, uint32_t *regs) {
  if (!current || current->state != PROC_RUNNABLE) return 0;

  uint32_t deliverable = current->sig_pending & ~current->sig_blocked;
  if (!deliverable) return 0;

  int sig = ctz32(deliverable);
  current->sig_pending &= ~(1u << sig);

  sighandler_t handler = current->sig_handlers[sig];
  if (handler == SIG_IGN) return 0;
  if (handler == SIG_DFL) {
    signal_default_action(sig, regs);
    return 0;
  }

  *sig_out = sig;
  return 1;
}

int signal_check_kernel(void) {
  uint32_t deliverable = current->sig_pending & ~current->sig_blocked;
  if (!deliverable) return 0;

  int sig = ctz32(deliverable);
  current->sig_pending &= ~(1u << sig);

  if (current->sig_handlers[sig] == SIG_IGN) return 1;

  return signal_default_action(sig, NULL);
}

/* ── sys_kill ───────────────────────────────────────────────────────────────
 */

long sys_kill(long pid, long sig) {
  if (sig < 0 || sig >= NSIG) return -(long)EINVAL;

  /* Find target process by PID */
  pcb_t *target = NULL;
  for (uint32_t i = 0; i < PROC_MAX; i++) {
    if (proc_table[i].state != PROC_FREE && proc_table[i].pid == (pid_t)pid) {
      target = &proc_table[i];
      break;
    }
  }

  if (!target) return -(long)ESRCH;

  /* Signal 0 is a validity check — don't deliver */
  if (sig == 0) return 0;

  /* Set pending bit */
  target->sig_pending |= (1u << sig);

  /* Wake the target if it is blocked/sleeping */
  if (target->state == PROC_BLOCKED || target->state == PROC_SLEEPING)
    target->state = PROC_RUNNABLE;

  return 0;
}

/* ── sys_sigaction (kernel-internal) ────────────────────────────────────────
 *
 * Legacy 3-arg variant.  No longer reachable from user-space — the
 * SYS_SIGACTION (0x0601) syscall number was retired with this commit.
 * It survives only because tests/kernel/ktest.c exercises the
 * handler-install / -query / SIGKILL-reject paths from kernel context
 * and would otherwise have to build a struct kernel_sigaction on the
 * stack to drive sys_rt_sigaction.  The raw `*out = ...` deref below
 * is safe in that single caller because old_ptr is a kernel-stack
 * address; do not reintroduce it on the user-facing path.
 */

long sys_sigaction(long sig, long handler, long old_ptr) {
  if (sig < 1 || sig >= NSIG) return -(long)EINVAL;

  /* SIGKILL and SIGSTOP cannot be caught or ignored */
  if (sig == SIGKILL || sig == SIGSTOP) return -(long)EINVAL;

  /* Return old handler if requested */
  if (old_ptr) {
    sighandler_t *out = (sighandler_t *)(uintptr_t)old_ptr;
    *out = current->sig_handlers[sig];
  }

  /* Install new handler */
  current->sig_handlers[sig] = (sighandler_t)(uintptr_t)handler;

  return 0;
}

/* ── sys_rt_sigaction ────────────────────────────────────────────────────── */
/*
 * musl's sigaction() calls rt_sigaction(sig, act, oact, sigsetsize).
 *
 * struct k_sigaction {
 *     void (*handler)(int);    // offset 0
 *     unsigned long sa_flags;  // offset 4
 *     void (*sa_restorer)(void); // offset 8
 *     unsigned long sa_mask[2];  // offset 12
 * };
 */
long sys_rt_sigaction(long sig, uintptr_t act_ptr, uintptr_t oact_ptr,
                      long sigsetsize) {
  struct kernel_sigaction user_act;
  struct kernel_sigaction user_oact;
  int rc;
  (void)sigsetsize;

  if (sig < 1 || sig >= NSIG) return -(long)EINVAL;
  if (sig == SIGKILL || sig == SIGSTOP) return -(long)EINVAL;

  /* Return old handler if requested */
  if (oact_ptr != 0u) {
    user_oact.handler = (uint32_t)(uintptr_t)current->sig_handlers[sig];
    user_oact.flags = 0;
    user_oact.restorer = (uint32_t)(uintptr_t)current->sig_restorers[sig];
    user_oact.mask[0] = 0;
    user_oact.mask[1] = 0;
    rc = sys_copy_to_user(oact_ptr, &user_oact, sizeof(user_oact));
    if (rc < 0) return (long)rc;
  }

  /* Install new handler */
  if (act_ptr != 0u) {
    rc = sys_copy_from_user(&user_act, act_ptr, sizeof(user_act));
    if (rc < 0) return (long)rc;
    current->sig_handlers[sig] = (sighandler_t)(uintptr_t)user_act.handler;
    current->sig_restorers[sig] = (sighandler_t)(uintptr_t)user_act.restorer;
    /* sa_flags and sa_mask are noted but not fully supported yet */
  }

  return 0;
}

/* ── sys_rt_sigprocmask ──────────────────────────────────────────────────── */
/*
 * Manipulate the signal mask.
 *   how: SIG_BLOCK(0), SIG_UNBLOCK(1), SIG_SETMASK(2)
 *   set/oset: pointer to 64-bit signal mask (we use low 32 bits)
 */
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

long sys_rt_sigprocmask(long how, uintptr_t set_ptr, uintptr_t oset_ptr,
                        long sigsetsize) {
  uint32_t user_set[2] = {0, 0};
  uint32_t user_old[2] = {0, 0};
  size_t mask_size = 4;
  int rc;

  if (sigsetsize >= 8) mask_size = 8;

  /* Return current mask if requested */
  if (oset_ptr != 0u) {
    user_old[0] = current->sig_blocked;
    rc = sys_copy_to_user(oset_ptr, user_old, mask_size);
    if (rc < 0) return (long)rc;
  }

  if (set_ptr == 0u) return 0;

  rc = sys_copy_from_user(user_set, set_ptr, mask_size);
  if (rc < 0) return (long)rc;

  switch (how) {
    case SIG_BLOCK:
      current->sig_blocked |= user_set[0];
      break;
    case SIG_UNBLOCK:
      current->sig_blocked &= ~user_set[0];
      break;
    case SIG_SETMASK:
      current->sig_blocked = user_set[0];
      break;
    default:
      return -(long)EINVAL;
  }

  return 0;
}
