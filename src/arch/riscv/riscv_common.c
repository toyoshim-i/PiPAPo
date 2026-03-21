/*
 * riscv_common.c — Shared RISC-V architecture state and timer
 *
 * Provides:
 *   - Context switch pending flag (checked by timer ISR)
 *   - Core ID accessor (single-core for now)
 *   - RP2350 RISC-V timer init and interrupt handler
 */

#include <stdint.h>
#include "cpu.h"

/* Context switch pending flag.
 * Set by arch_yield() (via sched_tick or sched_yield).
 * Checked by timer ISR before mret to perform the switch.
 * Same pattern as m68k_switch_pending. */
volatile uint32_t riscv_switch_pending = 0;

/* SysTick-equivalent counter — incremented by timer ISR.
 * Used by the scheduler for time-slice accounting. */
volatile uint32_t riscv_tick_count = 0;

/* Core ID — single-core initial port.
 * On RP2350, SIO_CPUID at 0xD0000000 works for both ARM and RISC-V cores.
 * Phase RV-6 (dual-core) will use the real SIO_CPUID register. */
uint32_t core_id(void)
{
    return 0;
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
    /* Enable timer at full system clock speed */
    SIO_MTIME_CTRL = MTIME_CTRL_EN | MTIME_CTRL_FULLSPEED;

    /* Set first deadline */
    uint64_t now = mtime_read();
    mtimecmp_write(now + RISCV_TICK_INTERVAL);

    /* Enable Machine Timer Interrupt */
    csr_set(mie, MIE_MTIE);

    /* Enable global interrupts (mstatus.MIE) */
    csr_set(mstatus, MSTATUS_MIE);
}

/*
 * riscv_timer_handler — called from trap.S on timer interrupt (mcause = 7).
 *
 * Resets mtimecmp for the next tick and increments the tick counter.
 * Writing mtimecmp automatically clears MTIP (no explicit acknowledge).
 */
void riscv_timer_handler(void)
{
    /* Set next deadline relative to current mtimecmp (not mtime) to
     * avoid drift.  If we've fallen behind, the next interrupt fires
     * immediately (mtime >= mtimecmp). */
    uint64_t cmp_lo = SIO_MTIMECMP;
    uint64_t cmp_hi = SIO_MTIMECMPH;
    uint64_t next = ((cmp_hi << 32) | cmp_lo) + RISCV_TICK_INTERVAL;
    mtimecmp_write(next);

    riscv_tick_count++;
}
