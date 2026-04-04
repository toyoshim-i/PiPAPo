/*
 * ioregs.h — Xtensa I/O register definitions
 *
 * On ESP32-S3, peripherals (UART, SPI, GPIO) are at 0x60000000+.
 * This header provides the REG() macro and any Xtensa-specific MMIO
 * definitions shared across kernel files.
 */

#ifndef PPAP_ARCH_XTENSA_IOREGS_H
#define PPAP_ARCH_XTENSA_IOREGS_H

#include <stdint.h>

/* ── Register access helper ─────────────────────────────────────────────── */

#ifndef REG
#define REG(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#endif

/* ESP32-S3 peripheral registers are defined in cpu.h (included via arch.h). */

#endif /* PPAP_ARCH_XTENSA_IOREGS_H */
