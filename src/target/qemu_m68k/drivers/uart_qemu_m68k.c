/*
 * uart_qemu_m68k.c — UART driver for QEMU virt m68k (Goldfish TTY)
 *
 * The QEMU virt m68k machine uses a Goldfish TTY device for serial I/O.
 *
 * Goldfish TTY register map (base = 0xFF008000):
 *   +0x00  PUT_CHAR       Write a single character (byte)
 *   +0x04  BYTES_READY    Number of bytes available to read
 *   +0x08  CMD            Command register
 *   +0x10  DATA_PTR       Pointer to data buffer
 *   +0x14  DATA_LEN       Length of data
 *   +0x18  DATA_PTR_HIGH  High 32 bits of data pointer (64-bit)
 *   +0x20  VERSION        Device version
 *
 * For Phase B we only need TX (uart_putc / uart_puts).
 * RX and IRQ mode will be added in Phase C.
 */

#include "drivers/uart.h"
#include <stdint.h>

/* ── Goldfish TTY registers ─────────────────────────────────────────────── */

#define GOLDFISH_TTY_BASE    0xFF008000u

#define TTY_PUT_CHAR     (*(volatile uint32_t *)(GOLDFISH_TTY_BASE + 0x00u))
#define TTY_BYTES_READY  (*(volatile uint32_t *)(GOLDFISH_TTY_BASE + 0x04u))
#define TTY_CMD          (*(volatile uint32_t *)(GOLDFISH_TTY_BASE + 0x08u))
#define TTY_DATA_PTR     (*(volatile uint32_t *)(GOLDFISH_TTY_BASE + 0x10u))
#define TTY_DATA_LEN     (*(volatile uint32_t *)(GOLDFISH_TTY_BASE + 0x14u))

#define TTY_CMD_READ_BUFFER   3u

/* ── Public API ─────────────────────────────────────────────────────────── */

void uart_init_console(void)
{
    /* Goldfish TTY requires no initialization — ready immediately */
}

void uart_putc(char c)
{
    TTY_PUT_CHAR = (uint32_t)(unsigned char)c;
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
    /* Goldfish TTY: TX is synchronous, no buffering */
}

void uart_reinit_133mhz(void)
{
    /* Not applicable — QEMU has no clock change */
}

void uart_init_irq(void)
{
    /* Phase C: enable Goldfish TTY RX interrupt via Goldfish PIC */
}

int uart_getc(void)
{
    if (TTY_BYTES_READY == 0)
        return -1;
    /* Read a single byte via the buffer interface */
    static char rx_byte;
    TTY_DATA_PTR = (uint32_t)(uintptr_t)&rx_byte;
    TTY_DATA_LEN = 1;
    TTY_CMD = TTY_CMD_READ_BUFFER;
    return (int)(unsigned char)rx_byte;
}

int uart_rx_avail(void)
{
    return (TTY_BYTES_READY > 0) ? 1 : 0;
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
