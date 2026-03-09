/*
 * m68k_common.c — Shared m68k architecture state and fault handling
 *
 * Provides:
 *   - Context switch pending flag
 *   - 68020 instruction emulator (extb, muls.l, mulu.l, divs.l, divu.l)
 *   - Crash handler: prints register dump, kills faulting user process
 */

#include <stdint.h>
#include "../kernel/proc/proc.h"
#include "../kernel/klog.h"

/* Context switch pending flag.
 * Set by arch_yield() (via sched_tick or sched_yield).
 * Checked by timer ISR before RTE to perform the switch. */
volatile uint32_t m68k_switch_pending = 0;

/* ── 68020 instruction emulator ─────────────────────────────────────────── *
 *
 * Called from m68k_illegal_handler (boot.S) when the CPU encounters an
 * instruction it doesn't support.  Emulates extb.l, muls.l, mulu.l,
 * divs.l, and divu.l using only 68000-compatible operations.
 *
 * CRITICAL: This code must NOT use C's int64_t/uint64_t multiply or
 * divide operators, as GCC will emit calls to __muldi3/__divdi3 from
 * libgcc, which themselves contain 68020 instructions → infinite loop.
 * All 64-bit arithmetic is done manually with 32-bit and 16-bit ops.
 *
 * Register frame layout:
 *   regs[0..7]   = d0-d7
 *   regs[8..14]  = a0-a6
 *   byte offset 60: SR (16-bit)
 *   byte offset 62: PC (32-bit)
 *
 * Returns 0 on success (PC advanced), -1 on failure (halt).
 */

/* 32×32 → 64 unsigned multiply using 16-bit multiplies.
 * No 64-bit C operators — only 32-bit shifts, adds, and 16-bit muls. */
static void umul32x32(uint32_t a, uint32_t b, uint32_t *hi, uint32_t *lo)
{
    uint32_t a_lo = a & 0xffff;
    uint32_t a_hi = a >> 16;
    uint32_t b_lo = b & 0xffff;
    uint32_t b_hi = b >> 16;

    uint32_t p0 = a_lo * b_lo;           /* partial products (each ≤ 32 bits) */
    uint32_t p1 = a_lo * b_hi;
    uint32_t p2 = a_hi * b_lo;
    uint32_t p3 = a_hi * b_hi;

    /* Accumulate: result = p3<<32 + (p1+p2)<<16 + p0 */
    uint32_t mid = p1 + p2;
    uint32_t carry_mid = (mid < p1) ? 1u : 0u;  /* carry from p1+p2 */

    uint32_t lo_result = p0 + (mid << 16);
    uint32_t carry_lo = (lo_result < p0) ? 1u : 0u;

    uint32_t hi_result = p3 + (mid >> 16) + (carry_mid << 16) + carry_lo;

    *lo = lo_result;
    *hi = hi_result;
}

/* 64÷32 → 32q,32r unsigned divide using shift-subtract.
 * dividend_hi:dividend_lo / divisor → quotient, remainder. */
static void udiv64x32(uint32_t dividend_hi, uint32_t dividend_lo,
                      uint32_t divisor,
                      uint32_t *quot_out, uint32_t *rem_out)
{
    if (divisor == 0) {
        *quot_out = 0;
        *rem_out = 0;
        return;
    }
    /* If high part is zero, simple 32÷32 */
    if (dividend_hi == 0) {
        *quot_out = dividend_lo / divisor;
        *rem_out  = dividend_lo % divisor;
        return;
    }
    /* Shift-subtract algorithm for 64÷32 */
    uint32_t quot = 0;
    uint32_t rem = 0;
    for (int i = 63; i >= 0; i--) {
        /* rem = (rem << 1) | bit i of dividend */
        rem <<= 1;
        if (i >= 32)
            rem |= (dividend_hi >> (i - 32)) & 1u;
        else
            rem |= (dividend_lo >> i) & 1u;
        if (rem >= divisor) {
            rem -= divisor;
            if (i < 32)
                quot |= (1u << i);
        }
    }
    *quot_out = quot;
    *rem_out = rem;
}

/* Read an effective-address operand value.
 * mode = bits 5-3 of the opcode, reg = bits 2-0.
 * *pc is advanced past any extension words consumed. */
/* Get address register value.  a0-a6 are in regs[8..14].
 * a7 (SP) is not saved by movem.l — compute from frame position:
 * original SP = regs base + 60 (sw frame) + 6 (exception frame) = +66 */
static uint32_t get_areg(uint32_t *regs, int reg)
{
    if (reg < 7)
        return regs[8 + reg];
    return (uint32_t)(uintptr_t)((uint8_t *)regs + 66);
}

static uint32_t ea_read32(uint32_t *regs, int mode, int reg, uint16_t **pc)
{
    switch (mode) {
    case 0: /* Dn */
        return regs[reg];
    case 2: /* (An) */
        return *(uint32_t *)(uintptr_t)get_areg(regs, reg);
    case 5: { /* (d16, An) */
        int16_t disp = (int16_t)*(*pc)++;
        return *(uint32_t *)(uintptr_t)(get_areg(regs, reg) + disp);
    }
    case 7:
        if (reg == 4) { /* #imm */
            uint32_t hi = *(*pc)++;
            uint32_t lo = *(*pc)++;
            return (hi << 16) | lo;
        }
        break;
    }
    return 0;
}

int m68k_emulate_insn(uint32_t *regs)
{
    uint32_t *pc_ptr = (uint32_t *)((uint8_t *)regs + 62);
    uint32_t pc = *pc_ptr;

    uint16_t opcode = *(uint16_t *)(uintptr_t)pc;
    uint16_t *ext = (uint16_t *)(uintptr_t)(pc + 2);

    /* ── extb.l Dn (0x49c0-0x49c7) ──────────────────────────────── */
    if (opcode >= 0x49c0 && opcode <= 0x49c7) {
        int n = opcode & 7;
        int8_t byte_val = (int8_t)(regs[n] & 0xff);
        regs[n] = (uint32_t)(int32_t)byte_val;
        *pc_ptr = pc + 2;
        return 0;
    }

    /* ── muls.l / mulu.l (opcode 0x4c00 + ea) ───────────────────── */
    if ((opcode & 0xffc0) == 0x4c00) {
        uint16_t extw = *ext;
        int is_signed = (extw >> 11) & 1;
        int dl = extw & 7;
        int dh = (extw >> 12) & 7;
        int is_64 = (extw >> 10) & 1;

        int ea_mode = (opcode >> 3) & 7;
        int ea_reg  = opcode & 7;
        uint16_t *eapc = ext + 1;
        uint32_t src = ea_read32(regs, ea_mode, ea_reg, &eapc);

        uint32_t a = regs[dl];
        uint32_t b = src;
        int negate = 0;

        if (is_signed) {
            if ((int32_t)a < 0) { a = (uint32_t)(-(int32_t)a); negate = 1; }
            if ((int32_t)b < 0) { b = (uint32_t)(-(int32_t)b); negate ^= 1; }
        }

        uint32_t res_hi, res_lo;
        umul32x32(a, b, &res_hi, &res_lo);

        if (is_signed && negate) {
            /* Negate 64-bit result: ~result + 1 */
            res_lo = ~res_lo + 1;
            res_hi = ~res_hi + (res_lo == 0 ? 1 : 0);
        }

        regs[dl] = res_lo;
        if (is_64)
            regs[dh] = res_hi;

        *pc_ptr = (uint32_t)(uintptr_t)eapc;
        return 0;
    }

    /* ── divs.l / divu.l (opcode 0x4c40 + ea) ───────────────────── */
    if ((opcode & 0xffc0) == 0x4c40) {
        uint16_t extw = *ext;
        int is_signed = (extw >> 11) & 1;
        int dq = extw & 7;
        int dr = (extw >> 12) & 7;
        int is_64 = (extw >> 10) & 1;

        int ea_mode = (opcode >> 3) & 7;
        int ea_reg  = opcode & 7;
        uint16_t *eapc = ext + 1;
        uint32_t divisor = ea_read32(regs, ea_mode, ea_reg, &eapc);

        if (divisor == 0)
            return -1;

        uint32_t dvd_hi, dvd_lo;
        int neg_q = 0, neg_r = 0;

        if (is_64) {
            dvd_hi = regs[dr];
            dvd_lo = regs[dq];
        } else {
            dvd_hi = 0;
            dvd_lo = regs[dq];
        }

        if (is_signed) {
            /* Negate dividend if negative */
            if ((int32_t)(is_64 ? dvd_hi : dvd_lo) < 0) {
                neg_q = 1;
                neg_r = 1;
                dvd_lo = ~dvd_lo + 1;
                dvd_hi = ~dvd_hi + (dvd_lo == 0 ? 1 : 0);
            }
            if ((int32_t)divisor < 0) {
                neg_q ^= 1;
                divisor = (uint32_t)(-(int32_t)divisor);
            }
        }

        uint32_t quot, rem;
        udiv64x32(dvd_hi, dvd_lo, divisor, &quot, &rem);

        if (is_signed) {
            if (neg_q) quot = (uint32_t)(-(int32_t)quot);
            if (neg_r) rem  = (uint32_t)(-(int32_t)rem);
        }

        regs[dq] = quot;
        regs[dr] = rem;

        *pc_ptr = (uint32_t)(uintptr_t)eapc;
        return 0;
    }

    klogf("EMU: UNHANDLED pc=%x op=%x", pc, (uint32_t)opcode);
    return -1;
}

/* ── Crash handler ─────────────────────────────────────────────────────── *
 *
 * Called from boot.S fault handlers with:
 *   fault_type: 68000 vector number (2=bus, 3=addr, 4=illegal, 5=zerodiv,
 *               6=CHK, 7=TRAPV, 8=privilege)
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
#define SIGILL  4
#define SIGBUS  7
#define SIGFPE  8
#define SIGSEGV 11
#define SIGABRT 6

/* Fault type → description and POSIX signal */
static const char *fault_name(int fault_type)
{
    switch (fault_type) {
    case 2:  return "SIGBUS (bus error)";
    case 3:  return "SIGBUS (address error)";
    case 4:  return "SIGILL (illegal instruction)";
    case 5:  return "SIGFPE (divide by zero)";
    case 6:  return "SIGFPE (CHK)";
    case 7:  return "SIGFPE (TRAPV)";
    case 8:  return "SIGSEGV (privilege violation)";
    default: return "SIG??? (unknown)";
    }
}

static int fault_signal(int fault_type)
{
    switch (fault_type) {
    case 2: case 3:  return SIGBUS;
    case 4:          return SIGILL;
    case 5: case 6: case 7: return SIGFPE;
    case 8:          return SIGSEGV;
    default:         return SIGABRT;
    }
}

/*
 * m68k_crash_handler — called from boot.S fault handlers
 *
 *   fault_type: 68000 vector number (2-8)
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
int m68k_crash_handler(int fault_type, uint32_t *regs)
{
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
    klogf("\n*** %s ***", fault_name(fault_type));
    if (p)
        klogf("  Process %u (%s)", (uint32_t)p->pid, p->comm);
    klogf("  PC=%x  SR=%x", pc, (uint32_t)sr);
    if (is_group0)
        klogf("  Fault addr=%x  FC=%x", fault_addr, (uint32_t)exc[0]);
    klogf("  d0=%x d1=%x d2=%x d3=%x",
          regs[0], regs[1], regs[2], regs[3]);
    klogf("  d4=%x d5=%x d6=%x d7=%x",
          regs[4], regs[5], regs[6], regs[7]);
    klogf("  a0=%x a1=%x a2=%x a3=%x",
          regs[8], regs[9], regs[10], regs[11]);
    klogf("  a4=%x a5=%x a6=%x",
          regs[12], regs[13], regs[14]);

    /* Kernel fault → unrecoverable */
    if (!p || p->pid == 0) {
        klogf("  Kernel fault — halting.");
        return 0;
    }

    /* Check if the process has a signal handler installed.
     * If so, post the signal and let signal_check() deliver it
     * on the next return to user-mode.  For now on m68k, signal
     * delivery is not yet implemented, so we fall through to kill.
     * TODO: when m68k signal delivery is implemented, return the
     * saved context to the process with the signal pending. */
    sighandler_t handler = p->sig_handlers[sig];
    if (handler != (sighandler_t)0 /* SIG_DFL */ &&
        handler != (sighandler_t)1 /* SIG_IGN */) {
        klogf("  Signal %u posted (handler at %x)",
              (uint32_t)sig, (uint32_t)(uintptr_t)handler);
        p->sig_pending |= (1u << sig);
        /* TODO: once m68k signal_check() can deliver signals,
         * return here and let the process resume with the signal
         * pending.  For now, fall through to kill. */
    }

    klogf("  Killed (exit status %u)", (uint32_t)(128 + sig));
    sys_exit(128 + sig);
    return 1;
}
