/*
 * timer_x68k.c — Timer driver stub for X68000 (Phase X-1)
 *
 * Phase X-1 is cooperative-only: no preemptive scheduling.
 * Real hardware timer support (X68000 MFP Timer-C at 100 Hz) is
 * implemented in Phase X-2.
 *
 * X68000 MFP (MC68901) Timer-C plan (Phase X-2):
 *   MFP base address: 0xE88000
 *   Timer-C input clock: 4 MHz / 200 prescaler = 20 kHz
 *   Timer-C data register: 200  → 20 kHz / 200 = 100 Hz
 *   Timer-C interrupt vector: MFP VR base + 10 (typically 0x4A = vector 74)
 *   → CPU IRQ level: MFP GPIP4 → Level 4 autovector
 */

#include "proc/sched.h"

/* ── Public API ─────────────────────────────────────────────────────────── */

void timer_init(void)
{
    /* Phase X-1 stub — MFP Timer-C initialization deferred to Phase X-2.
     * Scheduler runs cooperative-only (sched_yield() / sched_sleep()). */
}

/*
 * m68k_timer_handler_c — called from m68k_timer_isr (switch.S)
 *
 * Phase X-1 stub — never called because timer_init() does not enable
 * the MFP Timer-C interrupt.  Provided to satisfy the link-time symbol
 * reference from switch.S.
 */
void m68k_timer_handler_c(void)
{
    /* Phase X-1 stub — will tick the scheduler in Phase X-2 */
    sched_timer_tick(0);
}
