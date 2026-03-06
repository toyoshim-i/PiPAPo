/*
 * rp2040.h — RP2040 peripheral register definitions
 *
 * Shared header for hardware register addresses and bit definitions that
 * are used across multiple driver files (uart.c, clock.c, spi.c).
 *
 * Peripheral-specific registers that are only used in a single driver
 * (e.g., UART0 registers in uart.c, SPI0 in spi.c) remain local to that
 * file to avoid an overly large header.
 */

#ifndef PPAP_HW_RP2040_H
#define PPAP_HW_RP2040_H

#include <stdint.h>

/* ── Register access helper ─────────────────────────────────────────────── */

#define REG(addr)  (*(volatile uint32_t *)(uintptr_t)(addr))

/* ── RESETS peripheral (base 0x4000C000) ─────────────────────────────────
 *
 * Atomic register aliases (RP2040 bus fabric feature):
 *   +0x0000  normal R/W
 *   +0x1000  XOR
 *   +0x2000  SET  (write 1 to set bits  → hold peripheral in reset)
 *   +0x3000  CLR  (write 1 to clear bits → release peripheral from reset)
 * ────────────────────────────────────────────────────────────────────────── */

#define RESETS_RESET       REG(0x4000C000u)   /* reset control                */
#define RESETS_RESET_DONE  REG(0x4000C008u)   /* 1 = peripheral out of reset  */
#define RESETS_RESET_SET   REG(0x4000E000u)   /* SET alias: write 1 → assert  */
#define RESETS_RESET_CLR   REG(0x4000F000u)   /* CLR alias: write 1 → release */

/* Reset bit positions */
#define RESET_I2C1         (1u << 4)          /* I2C1 (DW_apb_i2c)            */
#define RESET_IO_BANK0     (1u << 5)          /* GPIO pin-function mux        */
#define RESET_PADS_BANK0   (1u << 8)          /* GPIO pad settings            */
#define RESET_PLL_SYS      (1u << 12)         /* PLL_SYS                      */
#define RESET_SPI0         (1u << 16)         /* SPI0 (PL022)                 */
#define RESET_SPI1         (1u << 17)         /* SPI1 (PL022)                 */
#define RESET_UART0        (1u << 22)         /* UART0 (PL011)                */

/* ── CLOCKS peripheral (base 0x40008000) ─────────────────────────────────
 *
 * Each clock generator occupies 12 bytes: CTRL (+0), DIV (+4), SELECTED (+8)
 *   clk_ref  offset 0x30  — reference clock fed to clk_sys
 *   clk_sys  offset 0x3C  — system clock (CPU, bus fabric)
 *   clk_peri offset 0x48  — peripheral clock (UART, SPI, I2C, …)
 * ────────────────────────────────────────────────────────────────────────── */

#define CLK_REF_CTRL      REG(0x40008030u)
#define CLK_REF_SELECTED  REG(0x40008038u)  /* one-hot: bit N = source N active */
#define CLK_SYS_CTRL      REG(0x4000803Cu)
#define CLK_SYS_SELECTED  REG(0x40008044u)
#define CLK_PERI_CTRL     REG(0x40008048u)

/* clk_ref SRC field [1:0]: 0=ROSC, 1=AUX, 2=XOSC */
#define CLK_REF_SRC_ROSC  0u
#define CLK_REF_SRC_XOSC  2u

/* clk_sys SRC bit [0]: 0=clk_ref (glitchless), 1=AUX */
#define CLK_SYS_SRC_REF   0u

/* clk_peri CTRL: AUXSRC[7:5]=0 (=clk_sys), ENABLE bit 11 */
#define CLK_PERI_ENABLE   (1u << 11)

#endif /* PPAP_HW_RP2040_H */
