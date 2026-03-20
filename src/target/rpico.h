/*
 * rpico.h — RP2040/RP2350 peripheral register definitions
 *
 * Shared header for hardware register addresses and bit definitions that
 * are used across multiple driver files (uart, clock, spi, i2c).
 * Base addresses come from the Pico SDK's chip-specific addressmap.h so
 * the same source works for both RP2040 and RP2350.
 *
 * Peripheral-specific registers that are only used in a single driver
 * (e.g., UART0 registers in uart.c, SPI0 in spi.c) remain local to that
 * file to avoid an overly large header.
 */

#ifndef PPAP_HW_RPICO_H
#define PPAP_HW_RPICO_H

#include <stdint.h>
#include <hardware/regs/addressmap.h>

/* ── Register access helper ─────────────────────────────────────────────── */

#define REG(addr)  (*(volatile uint32_t *)(uintptr_t)(addr))

/* ── RESETS peripheral ───────────────────────────────────────────────────
 *
 * Atomic register aliases (RP2040/RP2350 bus fabric feature):
 *   +0x0000  normal R/W
 *   +0x1000  XOR
 *   +0x2000  SET  (write 1 to set bits  → hold peripheral in reset)
 *   +0x3000  CLR  (write 1 to clear bits → release peripheral from reset)
 * ────────────────────────────────────────────────────────────────────────── */

#define RESETS_RESET       REG(RESETS_BASE + 0x00u)  /* reset control                */
#define RESETS_RESET_DONE  REG(RESETS_BASE + 0x08u)  /* 1 = peripheral out of reset  */
#define RESETS_RESET_SET   REG(RESETS_BASE + 0x2000u)/* SET alias: write 1 → assert  */
#define RESETS_RESET_CLR   REG(RESETS_BASE + 0x3000u)/* CLR alias: write 1 → release */

/* Reset bit positions — from SDK's hardware/regs/resets.h (differ RP2040 vs RP2350) */
#include <hardware/regs/resets.h>
#define RESET_I2C1         RESETS_RESET_I2C1_BITS
#define RESET_IO_BANK0     RESETS_RESET_IO_BANK0_BITS
#define RESET_PADS_BANK0   RESETS_RESET_PADS_BANK0_BITS
#define RESET_PLL_SYS      RESETS_RESET_PLL_SYS_BITS
#define RESET_SPI0         RESETS_RESET_SPI0_BITS
#define RESET_SPI1         RESETS_RESET_SPI1_BITS
#define RESET_UART0        RESETS_RESET_UART0_BITS

/* ── CLOCKS peripheral ───────────────────────────────────────────────────
 *
 * Each clock generator occupies 12 bytes: CTRL (+0), DIV (+4), SELECTED (+8)
 *   clk_ref  offset 0x30  — reference clock fed to clk_sys
 *   clk_sys  offset 0x3C  — system clock (CPU, bus fabric)
 *   clk_peri offset 0x48  — peripheral clock (UART, SPI, I2C, …)
 * ────────────────────────────────────────────────────────────────────────── */

#define CLK_REF_CTRL      REG(CLOCKS_BASE + 0x30u)
#define CLK_REF_SELECTED  REG(CLOCKS_BASE + 0x38u)  /* one-hot: bit N = source N active */
#define CLK_SYS_CTRL      REG(CLOCKS_BASE + 0x3Cu)
#define CLK_SYS_SELECTED  REG(CLOCKS_BASE + 0x44u)
#define CLK_PERI_CTRL     REG(CLOCKS_BASE + 0x48u)

/* clk_ref SRC field [1:0]: 0=ROSC, 1=AUX, 2=XOSC */
#define CLK_REF_SRC_ROSC  0u
#define CLK_REF_SRC_XOSC  2u

/* clk_sys SRC bit [0]: 0=clk_ref (glitchless), 1=AUX */
#define CLK_SYS_SRC_REF   0u

/* clk_peri CTRL: AUXSRC[7:5]=0 (=clk_sys), ENABLE bit 11 */
#define CLK_PERI_ENABLE   (1u << 11)

#endif /* PPAP_HW_RPICO_H */
