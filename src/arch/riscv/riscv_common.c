/*
 * riscv_common.c — Shared RISC-V architecture state and timer
 *
 * Provides:
 *   - Context switch pending flag (checked by timer ISR)
 *   - Core ID accessor (single-core for now)
 *   - RP2350 RISC-V timer init and interrupt handler
 *   - Context switch helper (riscv_do_switch)
 */

#include <stdint.h>
#include "cpu.h"
#include "klog.h"
#include "proc/proc.h"
#include "proc/sched.h"

/* Context switch pending flag.
 * Set by arch_yield() (via sched_tick or sched_yield).
 * Checked by timer ISR before mret to perform the switch.
 * Same pattern as m68k_switch_pending. */
volatile uint32_t riscv_switch_pending = 0;

/* SysTick-equivalent counter — incremented by timer ISR.
 * Used by the scheduler for time-slice accounting. */
volatile uint32_t riscv_tick_count = 0;

/* core_id() is provided as static inline in spinlock.h (via proc.h).
 * Phase RV-6 (dual-core) will use SIO_CPUID for real core detection. */

/* ── Exception handler ────────────────────────────────────────────────────── */

/*
 * riscv_exception_handler — called from trap.S on synchronous exceptions.
 * Prints fault info over UART (if UART is up) and halts.
 */
/*
 * Breadcrumb at fixed SRAM address for OpenOCD inspection.
 * 0x20005F00: 0xDEADBEEF (magic), mcause, mepc, mtval
 */
#define CRASH_LOG ((volatile uint32_t *)0x20005F00u)

static volatile int exception_reentry_guard;

void riscv_exception_handler(uint32_t mcause, uint32_t mepc, uint32_t mtval,
                             uint32_t saved_fp, uint32_t saved_sp)
{
    /* Disable all interrupts to prevent further traps */
    csr_clear(mstatus, MSTATUS_MIE);
    csr_clear(mie, MIE_MTIE);

    /* Re-entry guard: if the handler itself faults (e.g. during klogf
     * or backtrace walk), the nested trap must not restart output —
     * just spin so the first diagnostic remains visible. */
    if (exception_reentry_guard) {
        for (;;) __asm__ volatile("nop");
    }
    exception_reentry_guard = 1;

    /* Print bare diagnostic FIRST — before touching any pointers that
     * could fault (current, backtrace).  These three values come from
     * CSRs passed in registers and are always safe. */
    klogf("TRAP: cause=%lx mepc=%lx mtval=%lx sp=%lx\n",
          (unsigned long)mcause, (unsigned long)mepc, (unsigned long)mtval, (unsigned long)saved_sp);

    CRASH_LOG[0] = 0xDEADBEEFu;
    CRASH_LOG[1] = mcause;
    CRASH_LOG[2] = mepc;
    CRASH_LOG[3] = mtval;

    klogf("  pid=%lu fp=%lx\n",
          (unsigned long)(current ? (uint32_t)current->pid : 0xFFFFFFFFu),
          (unsigned long)saved_fp);

    /* Walk the frame pointer chain from the faulting context.
     * Each frame: [fp-4] = ra, [fp-8] = prev fp.
     * Requires -fno-omit-frame-pointer. */
    klog("  backtrace:\n");
    uintptr_t fp = saved_fp;
    for (uint32_t depth = 0; depth < 16 && fp >= 0x80000000u; depth++) {
        uintptr_t ra = *(uintptr_t *)(fp - 4);
        uintptr_t prev_fp = *(uintptr_t *)(fp - 8);
        klogf("    #%u ra=%lx fp=%lx\n", (unsigned)depth, (unsigned long)ra, (unsigned long)fp);
        if (prev_fp <= fp) break;
        fp = prev_fp;
    }

    /* Spin with NOP instead of WFI — WFI may gate the Hazard3 core clock,
     * making the debug module unresponsive.  A NOP loop keeps the core
     * running so OpenOCD can always halt and inspect crash state. */
    for (;;) __asm__ volatile("nop");
}

/* ── Timer ────────────────────────────────────────────────────────────────── */

/*
 * Read the 64-bit mtime counter safely (handle rollover of low half).
 */
static uint64_t mtime_read(void)
{
    uint32_t hi, lo, hi2;
    do {
        hi  = SIO_MTIMEH;
        lo  = SIO_MTIME;
        hi2 = SIO_MTIMEH;
    } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

/*
 * Write mtimecmp safely (avoids spurious interrupts during update).
 *
 * Sequence:
 *   1. Set low half to 0xFFFFFFFF (prevents premature match)
 *   2. Write new high half
 *   3. Write new low half
 */
static void mtimecmp_write(uint64_t val)
{
    SIO_MTIMECMP  = 0xFFFFFFFFu;
    SIO_MTIMECMPH = (uint32_t)(val >> 32);
    SIO_MTIMECMP  = (uint32_t)val;
}

/*
 * riscv_timer_init — configure the SIO timer for periodic 10ms ticks.
 *
 * Called from target_early_init() or kmain() after clock_init_pll().
 * The timer runs at the system clock (PPAP_SYS_HZ), set by
 * mtime_ctrl.FULLSPEED.
 */
void riscv_timer_init(void)
{
#ifndef PPAP_QEMU
    /* Enable timer at full system clock speed (RP2350 SIO-specific) */
    SIO_MTIME_CTRL = MTIME_CTRL_EN | MTIME_CTRL_FULLSPEED;
#endif
    /* QEMU CLINT timer runs automatically — no ctrl register needed */

    /* Set first deadline */
    uint64_t now = mtime_read();
    mtimecmp_write(now + RISCV_TICK_INTERVAL);

    /* Enable Machine Timer Interrupt */
    csr_set(mie, MIE_MTIE);

    /* Enable global interrupts (mstatus.MIE) */
    csr_set(mstatus, MSTATUS_MIE);
}

/* Forward declaration — sched_timer_tick() is in kernel/proc/sched.c. */
extern void sched_timer_tick(int from_user);

/*
 * riscv_timer_handler — called from trap.S on timer interrupt (mcause = 7).
 *
 * Resets mtimecmp for the next tick, increments the tick counter, and
 * calls sched_timer_tick() to drive time-slice accounting and preemption.
 * Writing mtimecmp automatically clears MTIP (no explicit acknowledge).
 *
 * The from_user flag is passed by trap.S: 1 if the interrupted code was
 * in U-mode, 0 if in M-mode.  (For now, everything runs in M-mode.)
 */
void riscv_timer_handler(int from_user)
{
    /* Set next deadline relative to current mtimecmp (not mtime) to
     * avoid drift.  If we've fallen behind, the next interrupt fires
     * immediately (mtime >= mtimecmp). */
    uint64_t cmp_lo = SIO_MTIMECMP;
    uint64_t cmp_hi = SIO_MTIMECMPH;
    uint64_t next = ((cmp_hi << 32) | cmp_lo) + RISCV_TICK_INTERVAL;
    mtimecmp_write(next);

    riscv_tick_count++;

    /* Drive scheduler time-slice accounting and preemption.
     * sched_timer_tick → sched_tick → arch_yield → riscv_switch_pending=1.
     * trap.S checks riscv_switch_pending on the return path. */
    sched_timer_tick(from_user);
}

/* ── Context switch ──────────────────────────────────────────────────────── */

/*
 * riscv_do_switch — called from trap.S when riscv_switch_pending is set.
 *
 * Saves the current trap-frame SP into the current pcb, calls sched_next()
 * to pick the next process, and returns the new process's saved SP.
 * trap.S then restores registers from the new SP's trap frame and mrets
 * into the new process.
 *
 * This follows the same pattern as the m68k port: the entire register set
 * is already saved in the trap frame by _trap_entry; we just swap the SP
 * pointer through the pcb.
 */
uint32_t riscv_do_switch(uint32_t current_sp)
{
    /* Save current SP (pointing to the trap frame) into the current pcb */
    if (current)
        current->sp = current_sp;

    /* Pick the next runnable process */
    pcb_t *next = sched_next();
    current_core[core_id()] = next;

    /* Ensure kernel_sp is set (may be 0 for pid 0 if sched_start ran
     * before the field was initialized, or for processes that were
     * created before the mscratch split was in place). */
    if (!next->kernel_sp && next->stack_page_id != PAGE_ID_INVALID)
        next->kernel_sp = (uint32_t)(uintptr_t)mm_page_to_ptr(next->stack_page_id) + PAGE_SIZE;

    return next->sp;
}

/* ── Initial stack frame for new processes ──────────────────────────────── */

uint32_t *arch_build_initial_frame(uint32_t *sp, void (*entry)(void))
{
    sp -= 36;  /* 36 words = 144 bytes = TRAP_FRAME_SIZE */
    for (int i = 0; i < 36; i++)
        sp[i] = 0u;
    /* gp (x3) at frame offset 1 */
    extern char __global_pointer$[];
    sp[1] = (uint32_t)(uintptr_t)__global_pointer$;
    /* mepc: entry point — mret jumps here */
    sp[30] = (uint32_t)(uintptr_t)entry;
    /* mstatus: MPP=U-mode (0), MPIE=1 (mret sets MIE from MPIE).
     * User processes run in U-mode; mret will drop to U-mode when MPP=0.
     * The kernel (pid 0) stays in M-mode — its frame is built differently
     * (sched_start sets mscratch directly, pid 0 never mrets to user). */
    sp[31] = (0u << 11) | (1u << 7);
    /* user_sp at offset 32 (= TF_USER_SP / 4) — set by caller */
    /* sp[32] = 0; — already zeroed, caller sets it via proc_setup_stack */
    return sp;
}
