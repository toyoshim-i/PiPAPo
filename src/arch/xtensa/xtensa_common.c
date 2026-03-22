/*
 * xtensa_common.c — Shared Xtensa architecture state
 *
 * Provides:
 *   - Context switch pending flag (checked by timer ISR)
 *   - Tick counter (stub for CC-1, wired in CC-2)
 *   - Exception handler (minimal, for debugging)
 *
 * Timer init and handler will be implemented in CC-2.
 */

#include <stdint.h>
#include "cpu.h"

/* Context switch pending flag.
 * Set by arch_yield() (via sched_tick or sched_yield).
 * Checked by timer ISR before returning to perform the switch.
 * Same pattern as riscv_switch_pending / m68k_switch_pending. */
volatile uint32_t xtensa_switch_pending = 0;

/* Tick counter — incremented by timer ISR (CC-2).
 * Used by the scheduler for time-slice accounting. */
volatile uint32_t xtensa_tick_count = 0;

/* ── Exception handler ────────────────────────────────────────────────────── */

/*
 * xtensa_exception_handler — called on unhandled exceptions.
 * Halts with interrupts disabled.  Attach a debugger to inspect.
 */
void xtensa_exception_handler(uint32_t exccause, uint32_t epc1,
                              uint32_t excvaddr)
{
    (void)exccause;
    (void)epc1;
    (void)excvaddr;

    /* Disable all interrupts */
    uint32_t dummy;
    __asm__ volatile ("rsil %0, 15" : "=a"(dummy));

    for (;;)
        __asm__ volatile ("waiti 15");
}
