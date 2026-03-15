/*
 * arm_m_common.c — ARM Cortex-M0+ fault handling
 *
 * Provides:
 *   - HardFault crash handler: prints register dump, kills faulting user process
 *
 * On Cortex-M0+, all faults (undefined instruction, bus error, alignment, etc.)
 * escalate to HardFault.  We inspect the faulting instruction to determine
 * whether it's an undefined opcode (SIGILL) or something else (SIGSEGV).
 */

#include <stdint.h>
#include "../kernel/proc/proc.h"
#include "../kernel/klog.h"
#include "../kernel/syscall/syscall.h"

/* POSIX signal numbers */
#define SIGILL  4
#define SIGBUS  7
#define SIGFPE  8
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
static int classify_fault(uint32_t pc)
{
    /* Faulting instruction — Thumb (16-bit).  The PC in the exception frame
     * points to the faulting instruction (bit 0 is always 0 in the frame). */
    uint16_t insn = *(volatile uint16_t *)(pc & ~1u);

    /* Check for permanently undefined Thumb encodings:
     * - 0xDExx: UDF #imm8 (ARMv6-M defined "undefined instruction" encoding)
     * - 0xB1x0..0xB1xF with certain bit patterns — rare, ignore for now
     *
     * The test_fault test uses .short 0xDEAD which falls in this range. */
    if ((insn & 0xFF00) == 0xDE00)
        return SIGILL;

    /* Everything else — best guess is memory access fault (SIGSEGV).
     * Cortex-M0+ HardFaults also fire for unaligned access, bus errors,
     * and other conditions we can't distinguish without CFSR. */
    return SIGSEGV;
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
void arm_crash_handler(uint32_t *psp_frame, uint32_t *callee_regs)
{
    if (trace_arm_hardfault_debug_stop(psp_frame))
        return;

    uint32_t pc   = psp_frame[6];
    uint32_t xpsr = psp_frame[7];

    pcb_t *p = current;
    int sig = classify_fault(pc);

    const char *signame;
    switch (sig) {
    case SIGILL:  signame = "SIGILL (illegal instruction)"; break;
    case SIGBUS:  signame = "SIGBUS (bus error)"; break;
    case SIGFPE:  signame = "SIGFPE (divide by zero)"; break;
    case SIGSEGV: signame = "SIGSEGV (segmentation fault)"; break;
    default:      signame = "SIG??? (unknown)"; break;
    }

    klogf("\n*** %s ***", signame);
    if (p)
        klogf("  Process %u (%s)", (uint32_t)p->pid, p->comm);
    klogf("  PC=%x  xPSR=%x", pc, xpsr);

    /* Print registers from the exception frame (r0-r3, r12, lr) */
    klogf("  r0=%x r1=%x r2=%x r3=%x",
          psp_frame[0], psp_frame[1], psp_frame[2], psp_frame[3]);
    klogf("  r12=%x lr=%x", psp_frame[4], psp_frame[5]);

    /* Print callee-saved registers: layout on MSP is {r8,r9,r10,r11,r4,r5,r6,r7} */
    klogf("  r4=%x r5=%x r6=%x r7=%x",
          callee_regs[4], callee_regs[5], callee_regs[6], callee_regs[7]);
    klogf("  r8=%x r9=%x r10=%x r11=%x",
          callee_regs[0], callee_regs[1], callee_regs[2], callee_regs[3]);

    /* Kernel fault — shouldn't reach here (boot.S branches to Default_Handler
     * for MSP faults), but guard anyway. */
    if (!p || p->pid == 0) {
        klogf("  Kernel fault — halting.");
        while (1) __asm volatile("" ::: "memory");
    }

    klogf("  Killed (exit status %u)", (uint32_t)(128 + sig));
    sys_exit(128 + sig);

    /* Patch the stacked PC so if PendSV doesn't tail-chain, the CPU won't
     * return to the faulting instruction and re-fault in an infinite loop.
     * Point to Default_Handler (infinite WFE loop) as a safe landing pad.
     * In practice PendSV always tail-chains, so this PC is never reached. */
    extern void Default_Handler(void);
    psp_frame[6] = (uint32_t)Default_Handler | 1u;  /* Thumb bit */
}
