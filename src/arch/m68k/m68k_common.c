/*
 * m68k_common.c — Shared m68k architecture state
 *
 * Provides definitions for variables declared extern in arch/m68k/arch.h.
 */

#include <stdint.h>

/* Context switch pending flag.
 * Set by arch_yield() (via sched_tick or sched_yield).
 * Checked by timer ISR before RTE to perform the switch. */
volatile uint32_t m68k_switch_pending = 0;
