/*
 * arm_m_common.c — ARM Cortex-M0+ fault handling
 *
 * Provides:
 *   - HardFault crash handler: prints register dump, kills faulting user
 * process
 *
 * On Cortex-M0+, all faults (undefined instruction, bus error, alignment, etc.)
 * escalate to HardFault.  We inspect the faulting instruction to determine
 * whether it's an undefined opcode (SIGILL) or something else (SIGSEGV).
 */

#include <stdint.h>

#include "kernel/core/klog.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/syscall/syscall.h"

/* POSIX signal numbers */
#define SIGILL 4
#define SIGBUS 7
#define SIGFPE 8
#define SIGSEGV 11

/*
 * Determine the fault type by inspecting the faulting Thumb instruction.
 *
 * Cortex-M0+ has no configurable fault status registers (CFSR/BFSR/MMFSR
 * are ARMv7-M only).  We heuristically classify by checking if the
 * instruction at the faulting PC is a permanently undefined encoding.
 *
 * Returns the POSIX signal number.
 */
static int classify_fault(uint32_t pc) {
  /* Faulting instruction — Thumb (16-bit).  The PC in the exception frame
   * points to the faulting instruction (bit 0 is always 0 in the frame). */
  uint16_t insn = *(volatile uint16_t *)(pc & ~1u);

  /* Check for permanently undefined Thumb encodings:
   * - 0xDExx: UDF #imm8 (ARMv6-M defined "undefined instruction" encoding)
   * - 0xB1x0..0xB1xF with certain bit patterns — rare, ignore for now
   *
   * The test_fault test uses .short 0xDEAD which falls in this range. */
  if ((insn & 0xFF00) == 0xDE00) return SIGILL;

  /* Everything else — best guess is memory access fault (SIGSEGV).
   * Cortex-M0+ HardFaults also fire for unaligned access, bus errors,
   * and other conditions we can't distinguish without CFSR. */
  return SIGSEGV;
}

/*
 * kernel_hardfault_dump — called from HardFault_Handler when fault is on MSP.
 *
 * msp_frame points to the hardware-saved exception frame on MSP:
 *   {r0, r1, r2, r3, r12, lr, pc, xpsr}
 */
/*
 * Polling UART output for crash context — klogf deadlocks in HardFault
 * because the UART ISR is masked (HardFault has highest priority).
 * These helpers write directly to the UART TX FIFO, waiting for space.
 */
static volatile uint32_t *const crash_uart_dr = (volatile uint32_t *)0x40034000u;
static volatile uint32_t *const crash_uart_fr = (volatile uint32_t *)0x40034018u;

static void crash_putc(char c) {
  while (*crash_uart_fr & (1u << 5)) ; /* wait while TX FIFO full */
  *crash_uart_dr = (uint32_t)c;
}

static void crash_puts(const char *s) {
  while (*s) {
    if (*s == '\n') crash_putc('\r');
    crash_putc(*s++);
  }
}

static void crash_hex32(uint32_t v) {
  crash_putc('0'); crash_putc('x');
  for (int i = 7; i >= 0; i--) {
    unsigned n = (v >> (i * 4)) & 0xFu;
    crash_putc(n < 10 ? (char)('0' + n) : (char)('a' + n - 10));
  }
}

static void crash_dec(uint32_t v) {
  char buf[10]; int i = 0;
  if (v == 0) { crash_putc('0'); return; }
  while (v) { buf[i++] = (char)('0' + v % 10); v /= 10; }
  while (--i >= 0) crash_putc(buf[i]);
}

void kernel_hardfault_dump(uint32_t *msp_frame) {
  extern uint32_t core_id(void);
  uint32_t pc = msp_frame[6];
  uint32_t lr = msp_frame[5];

  crash_puts("\n*** KERNEL HARDFAULT ***  Core ");
  crash_dec(core_id());
  crash_puts("\n  PC="); crash_hex32(pc);
  crash_puts("  LR="); crash_hex32(lr);
  crash_puts("  xPSR="); crash_hex32(msp_frame[7]);
  crash_puts("\n  r0="); crash_hex32(msp_frame[0]);
  crash_puts(" r1="); crash_hex32(msp_frame[1]);
  crash_puts(" r2="); crash_hex32(msp_frame[2]);
  crash_puts(" r3="); crash_hex32(msp_frame[3]);
  crash_puts("\n  r12="); crash_hex32(msp_frame[4]);

  pcb_t *p = current;
  if (p) {
    crash_puts("\n  current: pid="); crash_dec((uint32_t)p->pid);
    crash_puts(" ("); crash_puts(p->comm); crash_putc(')');
  }
  crash_puts("\n  Halting.\n");
}

/*
 * arm_crash_handler — called from HardFault_Handler (boot.S)
 *
 *   psp_frame:   pointer to the hardware-saved exception frame on PSP:
 *                {r0, r1, r2, r3, r12, lr, pc, xpsr}
 *   callee_regs: pointer to saved callee registers on MSP:
 *                {r8, r9, r10, r11, r4, r5, r6, r7}
 *
 * Prints a crash report and kills the faulting user process.
 * sys_exit() marks the process as ZOMBIE and pends PendSV for rescheduling.
 */
void arm_crash_handler(uint32_t *psp_frame, uint32_t *callee_regs) {
  if (trace_arm_hardfault_debug_stop(psp_frame)) return;

  uint32_t pc = psp_frame[6];
  uint32_t xpsr = psp_frame[7];

  pcb_t *p = current;
  int sig = classify_fault(pc);

  const char *signame;
  switch (sig) {
    case SIGILL:
      signame = "SIGILL (illegal instruction)";
      break;
    case SIGBUS:
      signame = "SIGBUS (bus error)";
      break;
    case SIGFPE:
      signame = "SIGFPE (divide by zero)";
      break;
    case SIGSEGV:
      signame = "SIGSEGV (segmentation fault)";
      break;
    default:
      signame = "SIG??? (unknown)";
      break;
  }

  klogf("\n*** %s ***", signame);
  if (p) klogf("  Process %u (%s)", (unsigned)p->pid, p->comm);
  klogf("  PC=%lx  xPSR=%lx", (unsigned long)pc, (unsigned long)xpsr);

  /* Print registers from the exception frame (r0-r3, r12, lr) */
  klogf("  r0=%lx r1=%lx r2=%lx r3=%lx", (unsigned long)psp_frame[0], (unsigned long)psp_frame[1],
        (unsigned long)psp_frame[2], (unsigned long)psp_frame[3]);
  klogf("  r12=%lx lr=%lx", (unsigned long)psp_frame[4], (unsigned long)psp_frame[5]);

  /* Print callee-saved registers: layout on MSP is {r8,r9,r10,r11,r4,r5,r6,r7}
   */
  klogf("  r4=%lx r5=%lx r6=%lx r7=%lx", (unsigned long)callee_regs[4], (unsigned long)callee_regs[5],
        (unsigned long)callee_regs[6], (unsigned long)callee_regs[7]);
  klogf("  r8=%lx r9=%lx r10=%lx r11=%lx", (unsigned long)callee_regs[0], (unsigned long)callee_regs[1],
        (unsigned long)callee_regs[2], (unsigned long)callee_regs[3]);

  /* Kernel fault: process 0 is the idle thread running on PSP. */
  if (!p || p->pid == 0) {
    klogf("  Kernel fault — halting.\n");
    while (1) __asm volatile("" ::: "memory");
  }

  klogf("  Killed (exit status %lu)", (unsigned long)(uint32_t)(128 + sig));
  sys_exit(128 + sig);

  /* Patch the stacked PC so if PendSV doesn't tail-chain, the CPU won't
   * return to the faulting instruction and re-fault in an infinite loop.
   * Point to Default_Handler (infinite WFE loop) as a safe landing pad.
   * In practice PendSV always tail-chains, so this PC is never reached. */
  extern void Default_Handler(void);
  psp_frame[6] = (uint32_t)Default_Handler | 1u; /* Thumb bit */
}

/* ── Initial stack frame for new processes ──────────────────────────────── */

#include "ioregs.h"

uint32_t *arch_build_initial_frame(uint32_t *sp, void (*entry)(void)) {
  /* Hardware exception frame (8 words, popped by EXC_RETURN) */
  *--sp = XPSR_THUMB_BIT;        /* xpsr: Thumb bit (T=1)       */
  *--sp = (uint32_t)entry & ~1u; /* pc: entry point (bit0 clear) */
  *--sp = EXC_RETURN_THREAD_PSP; /* lr: EXC_RETURN thread/PSP   */
  *--sp = 0u;                    /* r12                         */
  *--sp = 0u;                    /* r3                          */
  *--sp = 0u;                    /* r2                          */
  *--sp = 0u;                    /* r1                          */
  *--sp = 0u;                    /* r0                          */
  /* Software callee-saved frame (loaded by PendSV) */
#if __ARM_ARCH >= 8
  *--sp = EXC_RETURN_THREAD_PSP; /* EXC_RETURN (no FPU frame)   */
#endif
  *--sp = 0u;  /* r11 */
  *--sp = 0u;  /* r10 */
  *--sp = 0u;  /* r9  */
  *--sp = 0u;  /* r8  */
  *--sp = 0u;  /* r7  */
  *--sp = 0u;  /* r6  */
  *--sp = 0u;  /* r5  */
  *--sp = 0u;  /* r4 — pcb_t.sp points here */
  return sp;
}
