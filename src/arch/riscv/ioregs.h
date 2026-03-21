/*
 * ioregs.h — RISC-V I/O register definitions
 *
 * On RP2350, peripherals (UART, SPI, GPIO, PLL) are at the same addresses
 * regardless of CPU architecture.  ARM-specific PPB registers (SysTick, NVIC,
 * SCB) do not exist; the RISC-V port uses SIO mtime for ticks instead.
 *
 * This header provides the REG() macro and any RISC-V-specific MMIO
 * definitions shared across kernel files.
 */

#ifndef PPAP_ARCH_RISCV_IOREGS_H
#define PPAP_ARCH_RISCV_IOREGS_H

#include <stdint.h>

/* ── Register access helper ─────────────────────────────────────────────── */

#ifndef REG
#define REG(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#endif

/* SIO timer registers are defined in cpu.h (included via arch.h). */

#endif /* PPAP_ARCH_RISCV_IOREGS_H */
