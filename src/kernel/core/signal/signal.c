/*
 * signal.c — Signal infrastructure for PPAP
 *
 *   sys_kill(pid, sig)            — send signal to process
 *   sys_sigaction(sig, hdl, old)  — install/query signal handler
 *   sys_sigreturn()               — restore context after signal handler
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
#include "kernel/core/subsys/subsys.h"
#include "kernel/core/syscall/syscall.h"

struct kernel_sigaction {
  uint32_t handler;
  uint32_t flags;
  uint32_t restorer;
  uint32_t mask[2];
};

static int ctz32(uint32_t x) {
  int n = 0;
  if (!(x & 0xFFFF)) {
    n += 16;
    x >>= 16;
  }
  if (!(x & 0xFF)) {
    n += 8;
    x >>= 8;
  }
  if (!(x & 0xF)) {
    n += 4;
    x >>= 4;
  }
  if (!(x & 0x3)) {
    n += 2;
    x >>= 2;
  }
  if (!(x & 0x1)) {
    n += 1;
  }
  return n;
}

static int signal_default_action(int sig, uint32_t *regs) {
  if (sig == SIGCHLD) return 1;

  {
    const subsys_ops_t *ops =
        current->subsys < SUBSYS_MAX ? subsys_ops_table[current->subsys] : 0;
    if (ops && ops->on_signal && ops->on_signal(current, sig, regs)) return 1;
  }

  sys_exit(128 + sig);
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
 * ARM:  RTE-based, sa_restorer style.  signal_setup_frame pushes a new
 *       HW exception frame below PSP with LR = current->sig_restorers[sig]
 *       (a user-space trampoline registered via rt_sigaction — see
 *       src/arch/arm_m/user/syscall.S).  The CPU unwinds the exception
 *       into the handler; the handler's `bx lr` lands on the restorer
 *       which issues SYS_RT_SIGRETURN, and sys_sigreturn advances PSP
 *       past the sig-delivery frame.
 *
 * m68k: Synchronous call — signal_check calls the handler directly via
 *       m68k_call_signal_handler (assembly thunk that sets a5 = GOT base).
 *       No sigreturn needed; sys_sigreturn / sys_rt_sigreturn return -ENOSYS.
 *
 * i16:  IRET-based real-mode delivery, sa_restorer style.  trap.S passes
 *       (user_sp, user_ss) from the kernel-stack slot to signal_check(),
 *       which plants a new GP+IRET frame plus a small trailer on the
 *       user stack and returns the new user_sp.  The handler's near
 *       `ret` lands on the per-handler sa_restorer (e.g.
 *       _ppap_sigreturn_trampoline in src/arch/i16/user/syscall.S);
 *       that stub issues INT 30h SYS_RT_SIGRETURN, which restores the
 *       pre-signal user_sp in the kernel slot.  See the frame-layout
 *       comment on the i16 branch below.
 * ────────────────────────────────────────────────────────────────────────── */

#if defined(__m68k__)

/*
 * m68k signal delivery — synchronous call model
 *
 * On PPAP m68k, processes run in user mode with USP/SSP separation.
 * The ARM-style RTE-based signal delivery (push frame onto user stack,
 * modify exception frame, RTE to handler) is not used because the
 * m68000 lacks the full frame format needed for safe nested exceptions.
 *
 * Instead, signal_check() calls the handler directly as a C function via
 * an assembly thunk that sets a5 = GOT base (required for PIC/-msep-data).
 * The SSP register frame is saved/restored around the handler call so the
 * original context is preserved.
 *
 * This is equivalent to the RTE model in terms of observable behavior:
 * the handler runs synchronously on syscall return, just like ARM.
 */

#define SIGFRAME_SIZE 66 /* d0-d7/a0-a6 (60) + SR (2) + PC (4) */

/*
 * Trampoline stub — not used on m68k (synchronous delivery), but kept
 * so the linker doesn't complain about the extern declaration in signal.h.
 */
__attribute__((used, section(".text.sigreturn_trampoline"))) void
__asm_sigreturn_trampoline(void);
__asm(
    ".section .text.sigreturn_trampoline,\"ax\",@progbits\n"
    ".globl sigreturn_trampoline\n"
    "sigreturn_trampoline:\n"
    "    rts\n"
    ".previous\n");

/*
 * m68k_call_signal_handler — assembly thunk to call a PIC signal handler.
 *
 * Sets a5 = GOT base before calling the handler (required for -msep-data),
 * saves/restores the kernel's a5 around the call.
 *
 * Defined in signal_m68k.S.
 */
extern void m68k_call_signal_handler(sighandler_t handler, int sig,
                                     uint32_t got_base);

/* ── signal_check ───────────────────────────────────────────────────────────
 */

void signal_check(uint32_t *regs) {
  uint32_t deliverable = current->sig_pending & ~current->sig_blocked;
  if (!deliverable) return;

  int sig = ctz32(deliverable); /* lowest pending signal */
  current->sig_pending &= ~(1u << sig);

  sighandler_t handler = current->sig_handlers[sig];

  if (handler == SIG_IGN) return;

  if (handler == SIG_DFL) {
    signal_default_action(sig, regs);
    return;
  }

  /* Save SSP register frame (restored after handler returns) */
  uint8_t saved_frame[SIGFRAME_SIZE];
  __builtin_memcpy(saved_frame, regs, SIGFRAME_SIZE);

  /* Block signal during handler to prevent infinite recursion */
  uint32_t old_blocked = current->sig_blocked;
  current->sig_blocked |= (1u << sig);

  /* Call handler synchronously with correct GOT base */
  m68k_call_signal_handler(handler, sig, current->got_base);

  /* Restore SSP frame and signal mask */
  __builtin_memcpy(regs, saved_frame, SIGFRAME_SIZE);
  current->sig_blocked = old_blocked;
}

#elif defined(__ia16__)

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

#elif defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)

/* ── signal_setup_frame ─────────────────────────────────────────────────────
 */
/*
 * Build a signal delivery frame on the user stack.
 *
 * ARMv6-M (no FPU):
 *   [PSP - 32]  new 8-word HW frame (signal handler)
 *   [PSP]       original 8-word HW frame (sigframe)
 *   PSP set to PSP - 32.
 *
 * ARMv8-M (Cortex-M33, FPU):
 *   [PSP - 40]  new 8-word basic HW frame (signal handler)
 *   [PSP - 8]   saved EXC_RETURN (4 bytes) + padding (4 bytes)
 *   [PSP]       original HW frame (8 or 26 words depending on FPU)
 *   PSP set to PSP - 40.
 *   svc_exc_return forced to basic frame (bit 4 = 1).
 *
 * On exception return the CPU pops the new frame and runs the handler.
 * The handler's `bx lr` reaches the per-process sa_restorer the user
 * registered via rt_sigaction (typically _ppap_sigreturn_trampoline in
 * src/arch/arm_m/user/syscall.S), which issues SYS_RT_SIGRETURN.
 * sys_sigreturn reverses this: skips the sigreturn SVC frame, reads the
 * saved EXC_RETURN, and restores PSP to the original HW frame.
 */
static int signal_setup_frame(int sig, sighandler_t handler) {
  uint32_t psp;
  uint32_t restorer;
  __asm volatile("mrs %0, psp" : "=r"(psp));

  restorer = (uint32_t)(uintptr_t)current->sig_restorers[sig];
  if (restorer == 0u)
    return -1; /* no sa_restorer registered — cannot deliver safely */

#if __ARM_ARCH >= 8
  /* 8-word basic frame + 2-word saved EXC_RETURN slot = 40 bytes */
  uint32_t new_psp = psp - 40;
#else
  uint32_t new_psp = psp - 32;
#endif

  /* Bounds check: don't write below the stack page */
  uint32_t stack_base =
      (uint32_t)(uintptr_t)mem_region_page_to_ptr(current->stack_page_id);
  if (new_psp < stack_base)
    return -1; /* stack overflow — cannot deliver signal */

  uint32_t *frame = (uint32_t *)new_psp;

  frame[0] = (uint32_t)sig; /* r0 = signal number  */
  frame[1] = 0;             /* r1                  */
  frame[2] = 0;             /* r2                  */
  frame[3] = 0;             /* r3                  */
  frame[4] = 0;             /* r12                 */
  frame[5] = restorer;      /* lr = sa_restorer (Thumb bit set by linker) */
  frame[6] = (uint32_t)(uintptr_t)handler & ~1u; /* pc (bit0 clear)     */
  frame[7] = 0x01000000u;                        /* xpsr (Thumb bit)    */

#if __ARM_ARCH >= 8
  /* Save original EXC_RETURN between signal frame and original HW frame.
   * sys_sigreturn reads this to restore the correct frame type. */
  uint32_t cid = core_id();
  frame[8] = svc_exc_return[cid]; /* saved EXC_RETURN */
  frame[9] = 0;                   /* padding          */

  /* Force SVC_Handler to return with basic frame (bit 4 = 1).
   * The signal handler starts without FPU context. */
  svc_exc_return[cid] = svc_exc_return[cid] | 0x10u;
#endif

  __asm volatile("msr psp, %0" ::"r"(new_psp));
  return 0;
}

/* ── signal_check ───────────────────────────────────────────────────────────
 */

void signal_check(void) {
  uint32_t deliverable = current->sig_pending & ~current->sig_blocked;
  if (!deliverable) return;

  int sig = __builtin_ctz(deliverable); /* lowest pending signal */
  current->sig_pending &= ~(1u << sig);

  sighandler_t handler = current->sig_handlers[sig];

  if (handler == SIG_IGN) return;

  if (handler == SIG_DFL) {
    signal_default_action(sig, NULL);
    return;
  }

  /* User handler — set up trampoline frame on user stack */
  if (signal_setup_frame(sig, handler) < 0) {
    /* Stack overflow — cannot deliver signal, terminate process */
    sys_exit(128 + sig);
  }
}

#elif defined(__riscv)

/* RISC-V signal delivery — Phase RV-4 will implement. */

void signal_setup_frame(int sig) {
  /* Stub: deliver default action (terminate) for now. */
  if (current->sig_handlers[sig] == (sighandler_t)0 /* SIG_DFL */) {
    sys_exit(128 + sig);
  }
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

/* ── sys_sigaction ──────────────────────────────────────────────────────────
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

/* ── sys_sigreturn ──────────────────────────────────────────────────────────
 */

#if defined(__m68k__)

/* m68k uses synchronous signal delivery — no sigreturn needed.
 * These stubs exist for the syscall dispatch table. */
long sys_sigreturn(void) { return -(long)ENOSYS; }
long sys_rt_sigreturn(void) { return -(long)ENOSYS; }

#elif defined(__ia16__)

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

long sys_sigreturn(void) { return sys_rt_sigreturn(); }

#elif defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)

/*
 * Restore context after a signal handler returns via sigreturn_trampoline.
 *
 * ARMv6-M: PSP points to the sigreturn SVC's 8-word HW frame.
 *   Skip 32 bytes → original HW frame.
 *
 * ARMv8-M (M33, FPU): PSP points to the sigreturn SVC's HW frame,
 *   which may be basic (32 bytes) or extended (104 bytes) depending on
 *   whether the signal handler used FPU.  After skipping that frame,
 *   we read the saved EXC_RETURN (8 bytes), then PSP = original frame.
 *   We also restore svc_exc_return so SVC_Handler returns with the
 *   correct EXC_RETURN for the original frame type.
 */
long sys_sigreturn(void) {
  uint32_t psp;
  __asm volatile("mrs %0, psp" : "=r"(psp));

#if __ARM_ARCH >= 8
  uint32_t cid = core_id();
  uint32_t exc_ret = svc_exc_return[cid];

  /* Skip the sigreturn SVC's HW frame (basic or extended) */
  uint32_t frame_size = (exc_ret & 0x10u) ? 32u : 104u;
  psp += frame_size;

  /* Read saved EXC_RETURN and skip the 8-byte save slot */
  uint32_t *saved = (uint32_t *)psp;
  uint32_t orig_exc = saved[0];
  psp += 8;

  /* Restore original EXC_RETURN so SVC_Handler returns correctly */
  svc_exc_return[cid] = orig_exc;
#else
  psp += 32;
#endif

  __asm volatile("msr psp, %0" ::"r"(psp));
  return 0; /* value ignored — sigframe[0] has original r0 */
}

long sys_rt_sigreturn(void) { return sys_sigreturn(); /* same mechanism */ }

#elif defined(__riscv)

/* RISC-V sigreturn — Phase RV-4 will implement. */
long sys_sigreturn(void) { return -(long)ENOSYS; }
long sys_rt_sigreturn(void) { return -(long)ENOSYS; }

#elif defined(__xtensa__)

/* Xtensa sigreturn — CC-3 will implement. */
long sys_sigreturn(void) { return -(long)ENOSYS; }
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
