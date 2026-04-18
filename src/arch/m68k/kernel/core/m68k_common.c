/*
 * m68k_common.c — Shared m68k architecture state and fault handling
 *
 * Provides:
 *   - Context switch pending flag
 *   - Crash handler: prints register dump, kills faulting user process
 */

#include <stdint.h>

#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/subsys/subsys.h"
#include "kernel/core/syscall/syscall.h"
#ifdef PPAP_ENABLE_HUMAN68K
#include "kernel/core/subsys/human68k/human68k_bridge.h"
#endif

/* Pointer to the saved d0-d7/a0-a6/SR/PC frame at SSP, captured by
 * trap.S at TRAP #0 entry.  sys_rt_sigreturn reads it to find the SR
 * and PC it needs to overwrite so the RTE lands in the pre-signal
 * user context.  Zero outside a syscall trap. */
volatile uint32_t m68k_trap_frame_sp = 0;

/* ── Crash handler ─────────────────────────────────────────────────────── *
 *
 * Called from boot.S fault handlers with:
 *   fault_type: 68000 vector number (2=bus, 3=addr, 4=illegal, 5=zerodiv,
 *               6=CHK, 7=TRAPV, 8=privilege, 11=F-line)
 *   regs:       pointer to saved d0-d7/a0-a6 (60 bytes), followed by
 *               the 68000 exception frame (group 0: 14 bytes, group 1: 6 bytes)
 *
 * Prints a crash report with signal name, PC, fault address, and full
 * register dump.  If the faulting process is a user process (pid > 0),
 * kills it and returns 1 (caller reschedules).  If pid == 0 (kernel),
 * returns 0 (caller halts).
 */

/* Forward declarations */
long sys_exit(long status);
long sys_kill(long pid, long sig);

/* POSIX signal numbers */
#define SIGILL 4
#define SIGBUS 7
#define SIGFPE 8
#define SIGSEGV 11
#define SIGABRT 6

/* Fault type → description and POSIX signal */
static const char *fault_name(int fault_type) {
  switch (fault_type) {
    case 2:
      return "SIGBUS (bus error)";
    case 3:
      return "SIGBUS (address error)";
    case 4:
      return "SIGILL (illegal instruction)";
    case 5:
      return "SIGFPE (divide by zero)";
    case 6:
      return "SIGFPE (CHK)";
    case 7:
      return "SIGFPE (TRAPV)";
    case 8:
      return "SIGSEGV (privilege violation)";
    case 11:
      return "SIGILL (F-line)";
    default:
      return "SIG??? (unknown)";
  }
}

static int fault_signal(int fault_type) {
  switch (fault_type) {
    case 2:
    case 3:
      return SIGBUS;
    case 4:
    case 11:
      return SIGILL;
    case 5:
    case 6:
    case 7:
      return SIGFPE;
    case 8:
      return SIGSEGV;
    default:
      return SIGABRT;
  }
}

/*
 * m68k_crash_handler — called from boot.S fault handlers
 *
 *   fault_type: 68000 vector number (2-8, 11)
 *   regs:       saved d0-d7/a0-a6 (60 bytes), followed by exception frame
 *
 * Prints a crash report.  For user processes, delivers the corresponding
 * POSIX signal.  If the process has a handler installed, the signal is
 * posted (the process gets a chance to handle it on next signal_check).
 * If no handler (SIG_DFL), the process is killed immediately.
 *
 * Returns: 1 = process killed, caller should reschedule
 *          0 = kernel fault, caller should halt
 */
/* Defined in uart_x68k.c — inhibit TVRAM output to prevent double fault */
#ifdef PPAP_X68K
extern int uart_tvram_inhibit;
#endif

int m68k_crash_handler(int fault_type, uint32_t *regs) {
#ifdef PPAP_X68K
  /* Prevent klogf from calling IOCS _B_PUTC, which could double-fault
   * if the crash occurred inside IOCS itself.  Output goes only to the
   * serial mirror (_OUT232C). */
  uart_tvram_inhibit = 1;
#endif

  uint16_t *exc = (uint16_t *)((uint8_t *)regs + 60);
  uint32_t pc, fault_addr = 0;
  uint16_t sr;
  int is_group0 = (fault_type == 2 || fault_type == 3);

  if (is_group0) {
    fault_addr = ((uint32_t)exc[1] << 16) | exc[2];
    sr = exc[4];
    pc = ((uint32_t)exc[5] << 16) | exc[6];
  } else {
    sr = exc[0];
    pc = ((uint32_t)exc[1] << 16) | exc[2];
  }

  pcb_t *p = current;
  int sig = fault_signal(fault_type);

  /* Print crash report (klogf supports: %s %u %x %% only) */
  mod_vfs.klogf("\n*** %s ***", fault_name(fault_type));
  if (p) mod_vfs.klogf("  Process %u (%s)", (unsigned)p->pid, p->comm);
  mod_vfs.klogf("  PC=%lx  SR=%lx", (unsigned long)pc,
                (unsigned long)(uint32_t)sr);
  if (is_group0)
    mod_vfs.klogf("  Fault addr=%lx  FC=%lx", (unsigned long)fault_addr,
                  (unsigned long)(uint32_t)exc[0]);
  mod_vfs.klogf("  d0=%lx d1=%lx d2=%lx d3=%lx", (unsigned long)regs[0],
                (unsigned long)regs[1], (unsigned long)regs[2],
                (unsigned long)regs[3]);
  mod_vfs.klogf("  d4=%lx d5=%lx d6=%lx d7=%lx", (unsigned long)regs[4],
                (unsigned long)regs[5], (unsigned long)regs[6],
                (unsigned long)regs[7]);
  mod_vfs.klogf("  a0=%lx a1=%lx a2=%lx a3=%lx", (unsigned long)regs[8],
                (unsigned long)regs[9], (unsigned long)regs[10],
                (unsigned long)regs[11]);
  mod_vfs.klogf("  a4=%lx a5=%lx a6=%lx", (unsigned long)regs[12],
                (unsigned long)regs[13], (unsigned long)regs[14]);

  /* Kernel fault → unrecoverable */
  if (!p || p->pid == 0) {
    mod_vfs.klogf("  Kernel fault — halting.");
    kernel_panic_halt(1);
    return 0;
  }

  /* Subsystem crash hook — lets personality layers (e.g. Human68k _ERRJVC)
   * handle faults before the default POSIX signal path. */
  {
    const subsys_ops_t *ops =
        p->subsys < SUBSYS_MAX ? subsys_ops_table[p->subsys] : 0;
    if (ops && ops->on_crash) {
      int rc = ops->on_crash(p, regs, exc, is_group0);
      if (rc) return rc;
    }
  }

  /* Check if the process has a signal handler installed.
   * If so, post the signal and let signal_check() deliver it
   * synchronously on the next syscall return. */
  sighandler_t handler = p->sig_handlers[sig];
  if (handler != (sighandler_t)0 /* SIG_DFL */ &&
      handler != (sighandler_t)1 /* SIG_IGN */) {
    mod_vfs.klogf("  Signal %lu posted (handler at %lx)",
                  (unsigned long)(uint32_t)sig,
                  (unsigned long)(uintptr_t)handler);
    p->sig_pending |= (1u << sig);
    return 1; /* resume — signal_check will deliver the handler */
  }

  mod_vfs.klogf("  Killed (exit status %lu)",
                (unsigned long)(uint32_t)(128 + sig));
  sys_exit(128 + sig);
  return 1;
}

/*
 * m68k_fline_dispatch — called from boot.S F-line handler (vector 11)
 *
 *   regs: saved d0-d7/a0-a6 (60 bytes), followed by group-1 exception
 *         frame (SR + PC, 6 bytes).  The 16-bit F-line opcode is at
 *         the faulting PC.
 *   usp:  user stack pointer at time of the exception.
 *
 * Returns: 0 = kernel fault (halt)
 *          1 = process killed (schedule next)
 *          2 = DOS call handled (restore regs, rte)
 */
int m68k_fline_dispatch(uint32_t *regs, uint32_t usp) {
  pcb_t *p = current;

#ifdef PPAP_ENABLE_HUMAN68K
  if (p && p->subsys == SUBSYS_HUMAN68K) {
    /* Read the faulting PC from the exception frame.
     * PC is at byte offset 62 (60 bytes of regs + 2 bytes SR). */
    uint8_t *frame = (uint8_t *)regs;
    uint32_t pc = *(uint32_t *)(frame + 62);
    uint16_t opcode = *(volatile uint16_t *)(uintptr_t)pc;

    /* User code runs in user mode — USP has the DOS call arguments
     * that were pushed before the F-line instruction. */
    int rc = human68k_dos_dispatch(regs, usp, opcode);
    if (rc > 0) return rc; /* 1 = exited, 2 = handled */
    /* rc < 0: unhandled DOS call — fall through to crash */
  }
#else
  (void)p; /* Suppress unused variable warning */
#endif

  /* Non-Human68k process or unhandled: crash with SIGILL */
  return m68k_crash_handler(11, regs);
}

/* ── Initial stack frame for new processes ──────────────────────────────── */

#include "kernel/common/ioregs.h" /* SR_USER */

uint32_t *arch_build_initial_frame(uint32_t *sp, void (*entry)(void)) {
  *--sp = (uint32_t)entry;              /* PC (4 bytes)            */
  sp = (uint32_t *)((uint8_t *)sp - 2); /* back up 2 bytes for SR */
  *(uint16_t *)sp = SR_USER;            /* SR: user mode, IPL=0   */
  /* Software register frame (a6..a0, d7..d0, high → low).
   * Must match movem.l %d0-%d7/%a0-%a6 register order. */
  for (int i = 0; i < 15; i++) *--sp = 0u;
  return sp; /* pcb_t.sp points to d0 */
}
