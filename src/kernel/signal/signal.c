/*
 * signal.c — Signal infrastructure for PPAP
 *
 *   sys_kill(pid, sig)            — send signal to process
 *   sys_sigaction(sig, hdl, old)  — install/query signal handler
 *   sys_sigreturn()               — restore context after signal handler
 *   signal_check()                — called from SVC_Handler on return to user
 *   sigreturn_trampoline          — handler returns here (kernel .text)
 *
 * Signal delivery model:
 *   signal_check() is called from SVC_Handler after syscall_dispatch()
 *   when the process is RUNNABLE.  It delivers one pending signal per
 *   syscall return:
 *     - SIG_IGN: clear pending bit, done
 *     - SIG_DFL: SIGCHLD ignored, all others terminate via sys_exit(128+sig)
 *     - User handler: push a new HW exception frame below PSP (the original
 *       frame becomes "sigframe"), set PSP to new frame.  On exception return
 *       the CPU pops the new frame and runs the handler.  The handler returns
 *       via bx lr to sigreturn_trampoline, which does SVC SYS_SIGRETURN.
 *       sys_sigreturn advances PSP by 32 so the CPU pops the sigframe
 *       (original context) on exception return.
 */

#include "signal.h"

#include <stddef.h>

#include "../common/errno.h"
#include "../mm/mem_region.h"
#include "../proc/proc.h"
#include "../proc/sched.h"
#include "../subsys/subsys.h"
#include "../syscall/syscall.h"

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
 * ARM:  RTE-based — sigreturn_trampoline (naked ASM), signal_setup_frame
 *       (PSP manipulation), signal_check (delivers via HW exception frame).
 *
 * m68k: Synchronous call — signal_check calls the handler directly via
 *       m68k_call_signal_handler (assembly thunk that sets a5 = GOT base).
 *       No sigreturn needed; sys_sigreturn / sys_rt_sigreturn return -ENOSYS.
 *
 * i16:  RTE-based real-mode delivery. trap.S passes the saved 24-byte INT
 *       frame to signal_check(), which can swap SP to a synthetic handler
 *       frame. The handler returns with `ret` into sigreturn_trampoline,
 *       which issues INT 30h SYS_RT_SIGRETURN.
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

#define I16_SIGNAL_FRAME_WORDS 12u
#define I16_SIGNAL_FRAME_BYTES (I16_SIGNAL_FRAME_WORDS * 2u)
#define I16_SIGNAL_CALL_BYTES 4u /* ret IP + int arg */
#define I16_SIGNAL_DELIVERY_BYTES \
  (I16_SIGNAL_FRAME_BYTES + I16_SIGNAL_CALL_BYTES)
#define I16_FRAME_AX 8u
#define I16_FRAME_IP 9u
#define I16_FRAME_CS 10u

extern volatile uint16_t i16_trap_frame_sp;
extern volatile uint16_t i16_sigreturn_restore_sp;

__attribute__((used, section(".text.sigreturn_trampoline"))) void
__asm_sigreturn_trampoline(void);
__asm(
    ".section .text.sigreturn_trampoline,\"ax\",@progbits\n"
    ".globl sigreturn_trampoline\n"
    "sigreturn_trampoline:\n"
    "    addw $2, %sp\n"
    "    movw $0x0605, %ax\n"
    "    int  $0x30\n"
    "1:  jmp 1b\n"
    ".previous\n");

static uint16_t *signal_setup_frame_i16(uint16_t *frame, int sig,
                                        sighandler_t handler) {
  uintptr_t old_sp = (uintptr_t)frame;
  uintptr_t new_sp = old_sp - I16_SIGNAL_DELIVERY_BYTES;
  uint16_t *new_frame;
  uint16_t *call_frame;
  uintptr_t stack_base;

  stack_base = mem_region_page_linear(current->stack_page_id);
  if (new_sp < stack_base) return NULL;

  new_frame = (uint16_t *)new_sp;
  __builtin_memcpy(new_frame, frame, I16_SIGNAL_FRAME_BYTES);
  new_frame[I16_FRAME_IP] = (uint16_t)(uintptr_t)handler;
  new_frame[I16_FRAME_CS] = 0;

  call_frame = (uint16_t *)(new_sp + I16_SIGNAL_FRAME_BYTES);
  call_frame[0] = (uint16_t)(uintptr_t)sigreturn_trampoline;
  call_frame[1] = (uint16_t)sig;
  return new_frame;
}

uint16_t *signal_check(uint16_t *frame) {
  uint32_t deliverable;
  int sig;
  sighandler_t handler;
  uint16_t *new_frame;

  if (!frame || current->state != PROC_RUNNABLE) return frame;

  deliverable = current->sig_pending & ~current->sig_blocked;
  if (!deliverable) return frame;

  sig = ctz32(deliverable);
  current->sig_pending &= ~(1u << sig);

  handler = current->sig_handlers[sig];
  if (handler == SIG_IGN) return frame;

  if (handler == SIG_DFL) {
    signal_default_action(sig, NULL);
    return frame;
  }

  new_frame = signal_setup_frame_i16(frame, sig, handler);
  if (!new_frame) {
    sys_exit(128 + sig);
    return frame;
  }

  return new_frame;
}

#elif defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)

/*
 * Placed in kernel .text (flash XIP).  User-mode code can execute flash.
 * When the signal handler does bx lr, it lands here.
 * Uses SYS_RT_SIGRETURN (0x0605).
 */
__attribute__((naked, used, section(".text.sigreturn_trampoline"))) void
sigreturn_trampoline(void) {
  __asm volatile(
      "ldr  r7, =0x0605\n" /* SYS_RT_SIGRETURN */
      "svc  0\n"
      "b    .\n" /* should never reach */
  );
}

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
 * sys_sigreturn reverses this: skips the sigreturn SVC frame, reads the
 * saved EXC_RETURN, and restores PSP to the original HW frame.
 */
static int signal_setup_frame(int sig, sighandler_t handler) {
  uint32_t psp;
  __asm volatile("mrs %0, psp" : "=r"(psp));

#if __ARM_ARCH >= 8
  /* 8-word basic frame + 2-word saved EXC_RETURN slot = 40 bytes */
  uint32_t new_psp = psp - 40;
#else
  uint32_t new_psp = psp - 32;
#endif

  /* Bounds check: don't write below the stack page */
  uint32_t stack_base = (uint32_t)(uintptr_t)mem_region_page_to_ptr(current->stack_page_id);
  if (new_psp < stack_base)
    return -1; /* stack overflow — cannot deliver signal */

  uint32_t *frame = (uint32_t *)new_psp;

  frame[0] = (uint32_t)sig; /* r0 = signal number  */
  frame[1] = 0;             /* r1                  */
  frame[2] = 0;             /* r2                  */
  frame[3] = 0;             /* r3                  */
  frame[4] = 0;             /* r12                 */
  frame[5] = (uint32_t)(uintptr_t)sigreturn_trampoline; /* lr (Thumb bit set) */
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

long sys_sigreturn(void) {
  if (i16_trap_frame_sp == 0u) return -(long)EFAULT;
  i16_sigreturn_restore_sp =
      (uint16_t)(i16_trap_frame_sp + I16_SIGNAL_FRAME_BYTES);
  return 0;
}

long sys_rt_sigreturn(void) { return sys_sigreturn(); }

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
    user_oact.restorer = 0;
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
