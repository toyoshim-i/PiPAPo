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

#if defined(__ia16__)

/*
 * ia16 signal delivery — IRET-based, split-frame aware.
 *
 * Frame layout (all on the user stack, SS=proc_seg):
 *
 *   [new_user_SP + 0..17]   GP register frame  (ES, DS, BP, DI, SI, DX,
 *                           CX, BX, AX — 9 words, popped by trap.S on
 *                           the restore path)
 *   [new_user_SP + 18..23]  IRET frame (IP=handler, CS=proc_seg,
 *                           FLAGS=preserved from original)
 *   [new_user_SP + 24..25]  return IP for handler's near `ret`  ──┐
 *   [new_user_SP + 26..27]  signal number (handler's first arg)  │ trailer
 *   [new_user_SP + 28..29]  saved pre-signal user_SP              ─┘
 *
 * The return IP is the per-handler sa_restorer recorded in
 * current->sig_restorers[sig] by rt_sigaction.  User-space libc/crt
 * supplies the trampoline body — typically `_ppap_sigreturn_trampoline`
 * in src/arch/i16/user/syscall.S — which does
 * `addw $2, %sp; movw $SYS_RT_SIGRETURN, %ax; int $0x30` and lands us
 * in sys_rt_sigreturn with user_SP = new_user_SP + 4 (after trap.S's
 * own 18-byte GP-register push).  sys_rt_sigreturn reads the saved
 * pre-signal user_SP at new_user_SP + 28 and writes it back to the
 * kernel-stack slot so the final IRET resumes at the original
 * pre-signal context.
 */

#define I16_GP_FRAME_BYTES 18u  /* ES/DS/BP/DI/SI/DX/CX/BX/AX  (9 words) */
#define I16_IRET_FRAME_BYTES 6u /* IP/CS/FLAGS                          */
#define I16_FULL_FRAME_BYTES (I16_GP_FRAME_BYTES + I16_IRET_FRAME_BYTES)
#define I16_SIG_TRAILER_BYTES 6u /* ret_ip + sig_arg + saved_user_sp      */
#define I16_SIG_DELIVERY_BYTES (I16_FULL_FRAME_BYTES + I16_SIG_TRAILER_BYTES)

#define I16_USER_SEG_PAGES 16u
#define I16_USER_STACK_PAGE_BASE \
  ((uint16_t)((I16_USER_SEG_PAGES - 1u) * PAGE_SIZE))

extern volatile uint16_t i16_trap_frame_sp;

uint16_t signal_check(uint16_t user_sp, uint16_t user_ss) {
  uint32_t deliverable;
  int sig;
  sighandler_t handler;
  uint16_t new_sp;
  uint16_t trampoline;
  uint16_t orig_flags;

  if (!current || current->state != PROC_RUNNABLE) return user_sp;

  deliverable = current->sig_pending & ~current->sig_blocked;
  if (!deliverable) return user_sp;

  sig = ctz32(deliverable);
  current->sig_pending &= ~(1u << sig);

  handler = current->sig_handlers[sig];
  if (handler == SIG_IGN) return user_sp;

  if (handler == SIG_DFL) {
    signal_default_action(sig, NULL);
    return user_sp;
  }

  /* User handler — build a delivery frame below the current user_SP.
   * The near-return target is the per-handler sa_restorer registered
   * by rt_sigaction (user-space trampoline supplied by crt0/syscall.S
   * via the _ppap_sigreturn_trampoline symbol).  Treat a missing
   * restorer like stack overflow: we can't land anywhere sane after
   * the handler returns. */
  trampoline = (uint16_t)(uintptr_t)current->sig_restorers[sig];
  if (trampoline == 0u) {
    sys_exit(128 + sig);
    return user_sp;
  }
  if (user_sp < (uint16_t)(I16_USER_STACK_PAGE_BASE + I16_SIG_DELIVERY_BYTES)) {
    sys_exit(128 + sig);
    return user_sp;
  }
  new_sp = (uint16_t)(user_sp - I16_SIG_DELIVERY_BYTES);

  /* All user-stack reads/writes go through the portable sys_copy_*
   * helpers.  On ia16 those resolve the 16-bit near pointer through
   * current->image (proc_seg base + segment offset) and reach into
   * page storage via mem_region_page_read/write — no arch-specific
   * plumbing needed here. */

  /* Preserve FLAGS from the original IRET frame so the handler runs
   * with the same interrupt-enable state as the interrupted code. */
  sys_copy_from_user(&orig_flags,
                     (uintptr_t)(user_sp + I16_GP_FRAME_BYTES + 4u), 2);

  {
    uint16_t gp_words[9] = {
        user_ss, /* ES */
        user_ss, /* DS */
        0,       /* BP */
        0,       /* DI */
        0,       /* SI */
        0,       /* DX */
        0,       /* CX */
        0,       /* BX */
        0,       /* AX */
    };
    sys_copy_to_user((uintptr_t)new_sp, gp_words, sizeof(gp_words));
  }
  {
    uint16_t iret_words[3];
    iret_words[0] = (uint16_t)(uintptr_t)handler; /* IP */
    iret_words[1] = user_ss;                      /* CS = proc_seg */
    iret_words[2] = orig_flags;                   /* FLAGS */
    sys_copy_to_user((uintptr_t)(new_sp + I16_GP_FRAME_BYTES), iret_words,
                     sizeof(iret_words));
  }
  {
    uint16_t trailer[3];
    trailer[0] = trampoline;    /* handler's near-ret target */
    trailer[1] = (uint16_t)sig; /* handler's first argument  */
    trailer[2] = user_sp;       /* saved pre-signal user_SP  */
    sys_copy_to_user((uintptr_t)(new_sp + I16_FULL_FRAME_BYTES), trailer,
                     sizeof(trailer));
  }

  return new_sp;
}

#elif defined(__riscv)

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

#if defined(__ia16__)

/*
 * Unwind the sig-delivery frame the kernel planted below the original
 * user_SP.  At entry, the kernel-stack slot holds the post-trap user_SP
 * which sits I16_FULL_FRAME_BYTES (= 24) below the saved pre-signal
 * user_SP trailer word written by signal_check:
 *
 *   [current user_SP + 24]  saved_orig_user_SP  ← read back here
 *
 * Overwriting the kernel slot is sufficient — trap.S's .Lrestore path
 * will then IRET through the pre-signal IRET frame still sitting in
 * place on the user stack.
 */
long sys_rt_sigreturn(void) {
  uint16_t *slot;
  uint16_t user_sp;
  uint16_t saved_sp = 0;

  if (i16_trap_frame_sp == 0u) return -(long)EFAULT;

  slot = (uint16_t *)(uintptr_t)i16_trap_frame_sp;
  user_sp = slot[0];

  sys_copy_from_user(&saved_sp, (uintptr_t)(user_sp + I16_FULL_FRAME_BYTES), 2);
  slot[0] = saved_sp;
  return 0;
}

#elif defined(__riscv)

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
