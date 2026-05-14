/*
 * signal.c — Signal infrastructure for PPAP
 *
 *   sys_kill(pid, sig)            — send signal to process
 *   sys_rt_sigaction(sig, ...)    — install/query signal handler
 *                                   (legacy 3-arg sys_sigaction below
 *                                   survives only as a kernel-internal
 *                                   helper that ktest exercises)
 *   sys_rt_sigreturn()            — restore context after signal handler
 *   signal_check()                — called on return to user mode
 *
 * Signal delivery model:
 *   signal_check() runs on the syscall return path (ARM / ia16) or is
 *   driven by the trap handler that has a pointer to the saved register
 *   frame (m68k).  It delivers one pending, non-blocked signal per
 *   return:
 *     - SIG_IGN: clear pending bit, done
 *     - SIG_DFL: SIGCHLD ignored, all others terminate via sys_exit(128+sig)
 *     - User handler: arch-specific — see the per-arch branches below.
 */

#include "kernel/core/signal/signal.h"

#include <stddef.h>

#include "common/errno.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/proc/sched.h"
#include "kernel/core/signal/signal_helper.h"
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

/* ── Architecture-specific signal delivery ─────────────────────────────────
 *
 * All arches share the sa_restorer model: user-space libc/crt provides
 * the sigreturn trampoline, the kernel records its address per handler
 * via rt_sigaction, and signal_check uses it as the handler's return
 * target.  Only the frame-building mechanics are arch-specific.
 *
 * ARM:  RTE-based.  signal_setup_frame pushes a new HW exception frame
 *       below PSP with LR = current->sig_restorers[sig].  The CPU unwinds
 *       into the handler; `bx lr` lands on the restorer, which issues
 *       SYS_RT_SIGRETURN.  sys_rt_sigreturn advances PSP past the
 *       sig-delivery frame.
 *
 * m68k: RTE-based.  signal_check rewrites the (SR, PC) slot in the trap
 *       frame so rte enters the handler in user mode, and plants a small
 *       sig-arg + saved-context trailer below USP.  Handler's `rts`
 *       lands on the restorer (which pops the sig arg slot and issues
 *       SYS_RT_SIGRETURN).  sys_rt_sigreturn reads the saved (SR, PC,
 *       USP) out of the trailer and rewrites the current trap frame so
 *       the trap-exit rte resumes at the pre-signal user PC.
 *
 * i16:  IRET-based real-mode delivery.  trap.S passes (user_sp, user_ss)
 *       from the kernel-stack slot to signal_check(), which plants a new
 *       GP+IRET frame plus a small trailer on the user stack and returns
 *       the new user_sp.  Handler's near `ret` lands on the restorer in
 *       proc_seg; that stub issues INT 30h SYS_RT_SIGRETURN, which
 *       restores the pre-signal user_sp in the kernel slot.  See the
 *       frame-layout comment on the i16 branch below.
 * ────────────────────────────────────────────────────────────────────────── */

#if defined(__riscv)

/*
 * RISC-V signal delivery — mret-based, sa_restorer style.
 *
 * Trap frame layout (144 bytes, see src/arch/riscv/kernel/core/trap.S):
 *
 *   offset 0   x1   (ra)       regs[0]
 *   offset 4   x3   (gp)
 *   offset 8   x4   (tp)
 *   offset 12  x5   (t0)
 *   offset 16  x6   (t1)
 *   offset 20  x7   (t2)
 *   offset 24  x8   (s0)
 *   offset 28  x9   (s1)
 *   offset 32  x10  (a0)       regs[8]
 *   ...
 *   offset 60  x17  (a7)
 *   ...
 *   offset 120 mepc             regs[30]
 *   offset 124 mstatus          regs[31]
 *   offset 128 user_sp          regs[32]
 *
 * signal_check plants a 144-byte copy of the trap frame below the
 * current user sp and rewrites the live frame so the trap-exit mret
 * lands in the handler with the right ABI state:
 *
 *   regs[mepc]    = handler
 *   regs[a0]      = sig         (first arg per RISC-V ABI)
 *   regs[ra]      = sa_restorer (handler's `ret` jumps here)
 *   regs[user_sp] = new_sp      (points just above the saved frame)
 *
 * Handler returns via `ret` → restorer → ecall SYS_RT_SIGRETURN.
 * sys_rt_sigreturn reads the saved frame back from user memory and
 * copies it over the current (sigreturn) trap frame so the next
 * trap-exit mret resumes the interrupted user PC with its original
 * register state.  It returns orig_a0 so syscall_dispatch writes back
 * the interrupted syscall's original return value (not 0).
 */

#define RV32_TF_RA_IDX 0u
#define RV32_TF_A0_IDX 8u
#define RV32_TF_MEPC_IDX 30u
#define RV32_TF_USER_SP_IDX 32u
#define RV32_TRAP_FRAME_SIZE 144u /* matches TRAP_FRAME_SIZE in trap.S */

extern volatile uint32_t rv32_trap_frame_sp;

void signal_check(uint32_t *regs) {
  uint32_t deliverable;
  int sig;
  sighandler_t handler;
  uint32_t restorer;
  uint32_t orig_sp;
  uint32_t new_sp;

  if (!current || current->state != PROC_RUNNABLE) return;

  deliverable = current->sig_pending & ~current->sig_blocked;
  if (!deliverable) return;

  sig = ctz32(deliverable);
  current->sig_pending &= ~(1u << sig);

  handler = current->sig_handlers[sig];
  if (handler == SIG_IGN) return;
  if (handler == SIG_DFL) {
    signal_default_action(sig, regs);
    return;
  }

  restorer = (uint32_t)(uintptr_t)current->sig_restorers[sig];
  if (restorer == 0u) {
    sys_exit(128 + sig);
    return;
  }

  orig_sp = regs[RV32_TF_USER_SP_IDX];
  new_sp = orig_sp - RV32_TRAP_FRAME_SIZE;
  /* RISC-V ABI requires 16-byte sp alignment at function entry.  The
   * trap frame size is already 16-byte aligned (144), so new_sp keeps
   * the alignment of orig_sp. */

  /* Save the interrupted frame verbatim on the user stack.  On
   * sys_rt_sigreturn we copy it back into the then-live trap frame,
   * which restores every caller-saved register to its pre-signal
   * value (callee-saved regs are preserved by the handler's ABI
   * compliance, just like ARM). */
  sys_copy_to_user((uintptr_t)new_sp, regs, RV32_TRAP_FRAME_SIZE);

  /* Rewrite live frame so mret enters the handler in user mode. */
  regs[RV32_TF_MEPC_IDX] = (uint32_t)(uintptr_t)handler;
  regs[RV32_TF_A0_IDX] = (uint32_t)sig;
  regs[RV32_TF_RA_IDX] = restorer;
  regs[RV32_TF_USER_SP_IDX] = new_sp;
}

#elif defined(__xtensa__)

/* Xtensa signal delivery — CC-3 will implement. */

void signal_setup_frame(int sig) {
  /* Stub: deliver default action (terminate) for now. */
  if (current->sig_handlers[sig] == (sighandler_t)0 /* SIG_DFL */) {
    sys_exit(128 + sig);
  }
}

#endif /* __m68k__ / ARM / __riscv / __xtensa__ */

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

/* ── sys_rt_sigreturn ──────────────────────────────────────────────────────
 */

#if defined(__riscv)

/*
 * Unwind the delivery frame planted by signal_check.
 *
 * At entry we are inside the restorer's ecall trap.  rv32_trap_frame_sp
 * points at the live trap frame; current user sp (in that frame) equals
 * new_sp — handler's `ret` doesn't touch sp and the restorer hasn't
 * adjusted it either.  The saved pre-signal frame sits at [new_sp ..
 * new_sp + 144).
 *
 * Copying it back over the live trap frame restores every caller-saved
 * register, mepc, mstatus, and user_sp to their pre-signal values.  We
 * then return orig_a0 so syscall_dispatch writes it into the a0 slot,
 * matching what trap-exit would restore anyway (no net change).
 */
long sys_rt_sigreturn(void) {
  uint32_t *regs;
  uint32_t saved_sp;
  uint32_t orig_a0;

  if (rv32_trap_frame_sp == 0u) return -(long)EFAULT;

  regs = (uint32_t *)(uintptr_t)rv32_trap_frame_sp;
  saved_sp = regs[RV32_TF_USER_SP_IDX];
  sys_copy_from_user(regs, (uintptr_t)saved_sp, RV32_TRAP_FRAME_SIZE);
  orig_a0 = regs[RV32_TF_A0_IDX];
  return (long)(int32_t)orig_a0;
}

#elif defined(__xtensa__)

/* Xtensa sigreturn — CC-3 will implement. */
long sys_rt_sigreturn(void) { return -(long)ENOSYS; }

#endif /* __m68k__ / ARM / __riscv / __xtensa__ */

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
