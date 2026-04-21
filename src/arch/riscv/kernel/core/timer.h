/*
 * timer.h -- RISC-V mtimer / mtimecmp driver API
 */

#ifndef PPAP_ARCH_RISCV_KERNEL_CORE_TIMER_H
#define PPAP_ARCH_RISCV_KERNEL_CORE_TIMER_H

/* Program the RISC-V mtime/mtimecmp CSRs and mstatus.MIE for periodic
 * scheduler ticks.  Targets call this during late_init. */
void riscv_timer_init(void);

#endif /* PPAP_ARCH_RISCV_KERNEL_CORE_TIMER_H */
