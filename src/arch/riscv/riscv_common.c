/*
 * riscv_common.c — Shared RISC-V architecture state
 *
 * Provides:
 *   - Context switch pending flag (checked by timer ISR)
 *   - Core ID accessor (single-core for now)
 */

#include <stdint.h>

/* Context switch pending flag.
 * Set by arch_yield() (via sched_tick or sched_yield).
 * Checked by timer ISR before mret to perform the switch.
 * Same pattern as m68k_switch_pending. */
volatile uint32_t riscv_switch_pending = 0;

/* Core ID — single-core initial port.
 * On RP2350, SIO_CPUID at 0xD0000000 works for both ARM and RISC-V cores.
 * Phase RV-6 (dual-core) will use the real SIO_CPUID register. */
uint32_t core_id(void)
{
    return 0;
}
