/*
 * uart_qemu.c — UART driver for QEMU mps2-an500 (CMSDK UART)
 *
 * Implements the same API as uart.c but targets the CMSDK UART0 at
 * 0x40004000 (ARM CoreLink SSE-050 Subsystem / mps2-an500 peripheral map).
 *
 * CMSDK UART register map (offsets from UART_BASE = 0x40004000):
 *   +0x00  DATA      [7:0] TX byte (write) / RX byte (read)
 *   +0x04  STATE     [0] TXFULL — wait if set before writing DATA
 *                    [1] RXFULL — data ready to read
 *   +0x08  CTRL      [0] TX_EN, [1] RX_EN, [2] TX_INT_EN, [3] RX_INT_EN
 *   +0x0C  INTSTATUS [0] TX interrupt, [1] RX interrupt (write-1-to-clear)
 *   +0x10  BAUDDIV   [19:0] baud rate = clk / BAUDDIV
 *
 * mps2-an500 system clock: 25 MHz
 *   115200 bps → BAUDDIV = 25000000 / 115200 ≈ 217
 *
 * In QEMU, the CMSDK UART never actually blocks (TXFULL is always 0) and
 * data written to DATA appears on the QEMU serial output immediately.
 * The init steps are included for correctness on real mps2 hardware.
 *
 * uart_flush() and uart_reinit_133mhz() are no-ops: QEMU has no clock
 * switch and the CMSDK UART has no busy flag.
 */

#include "uart.h"
#include <stdint.h>

/* ==========================================================================
 * Register access helper
 * ========================================================================== */

#define REG(addr)  (*(volatile uint32_t *)(uintptr_t)(addr))

/* ==========================================================================
 * CMSDK UART0 — base 0x40004000
 * ========================================================================== */

#define UART_BASE    0x40004000u

#define UART_DATA    REG(UART_BASE + 0x00u)
#define UART_STATE   REG(UART_BASE + 0x04u)
#define UART_CTRL    REG(UART_BASE + 0x08u)
#define UART_BAUDDIV REG(UART_BASE + 0x10u)

#define UART_STATE_TXFULL  (1u << 0)  /* TX buffer full — wait before writing */
#define UART_STATE_RXFULL  (1u << 1)  /* RX byte ready to read               */
#define UART_CTRL_TX_EN    (1u << 0)  /* enable transmitter */
#define UART_CTRL_RX_EN    (1u << 1)  /* enable receiver */

/* 25 MHz system clock / 115200 bps */
#define UART_BAUDDIV_115200  217u

/* ==========================================================================
 * Public API
 * ========================================================================== */

void uart_init_console(void)
{
    UART_BAUDDIV = UART_BAUDDIV_115200;
    UART_CTRL    = UART_CTRL_TX_EN | UART_CTRL_RX_EN;
}

void uart_putc(char c)
{
    /* QEMU CMSDK UART: TXFULL is always 0 in the emulator, so no polling
     * needed.  Skipping the busy-wait avoids potential deadlocks when
     * called from SVC handler context (e.g., user-space sys_write). */
    UART_DATA = (uint32_t)(unsigned char)c;
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_flush(void)
{
    /* CMSDK UART has no shift-register busy flag; TX is synchronous in QEMU */
}

void uart_reinit_133mhz(void)
{
    /* No clock change in QEMU — baud rate stays at 25 MHz / 217 */
}

void uart_init_irq(void)
{
    /* CMSDK UART has no FIFO interrupt support in QEMU.
     * TX is already effectively non-blocking (TXFULL never set in QEMU).
     * Leave in polling mode — no IRQ setup needed. */
}

int uart_getc(void)
{
    if (UART_STATE & UART_STATE_RXFULL)
        return (int)(unsigned char)UART_DATA;
    return -1;
}

void uart_print_hex32(uint32_t v)
{
    uart_puts("0x");
    for (int i = 7; i >= 0; i--) {
        unsigned nibble = (v >> (i * 4)) & 0xFu;
        uart_putc(nibble < 10u ? (char)('0' + nibble) : (char)('a' + nibble - 10u));
    }
}

void uart_print_dec(uint32_t v)
{
    char buf[10];
    int  i = 0;
    if (v == 0u) { uart_putc('0'); return; }
    while (v > 0u) { buf[i++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (--i >= 0) uart_putc(buf[i]);
}
