/*
 * uart_x68k.c — Console driver for X68000 via IPL IOCS
 *
 * Implements the uart.h interface using X68000 IOCS calls (TRAP #15).
 * Stage2 preserves the IPL IOCS handler at vector 47 (TRAP #15), so all
 * IOCS function codes work transparently from supervisor mode.
 *
 * IOCS functions used:
 *   _B_PUTC   (d0=0x20, d1.b=char)  Output one character to TVRAM console
 *   _B_GETC   (d0=0x21)             Input one character (blocking)
 *   _B_KEYSNS (d0=0x1C)             Keyboard sense (non-blocking)
 *   _OUT232C  (d0=0x35, d1.b=char)  Output one character to RS-232C serial
 *
 * TX output goes to both the TVRAM text console and the RS-232C serial port.
 * In XEiJ, the RS-232C serial port output appears on stdio (the terminal).
 * RX input comes from the keyboard controller.
 */

#include "drivers/uart.h"
#include <stdint.h>

/* ── IOCS helper macros ──────────────────────────────────────────────────── */

/*
 * iocs_set232c — initialize RS-232C via _SET232C (IOCS 0x32)
 *
 * d0 = 0x32 (function code)
 * d1 = baud rate (bits 3-0): 7 = 9600 bps
 * d2 = character format: bits 1-0 = word length (3=8bit),
 *                        bits 3-2 = parity (0=none),
 *                        bits 5-4 = stop bits (0=1 stop)
 * → d1=0x07, d2=0x03 for 9600 8N1
 */
static inline void iocs_set232c(void)
{
    register int32_t d0 asm("d0") = 0x32;
    register int32_t d1 asm("d1") = 0x07;   /* 9600 baud */
    register int32_t d2 asm("d2") = 0x03;   /* 8N1 */
    asm volatile("trap #15"
                 : "+r"(d0)
                 : "r"(d1), "r"(d2)
                 : "a0", "a1", "memory");
}

/*
 * iocs_b_putc — output one character via _B_PUTC (IOCS 0x20)
 *
 * d0 = 0x20 (function code)
 * d1 = character (low byte)
 * Clobbers: d0, d2, a0, a1 (per X68000 IOCS calling convention)
 */
static inline void iocs_b_putc(char c)
{
    register int32_t d0 asm("d0") = 0x20;
    register int32_t d1 asm("d1") = (unsigned char)c;
    asm volatile("trap #15"
                 : "+r"(d0)
                 : "r"(d1)
                 : "d2", "a0", "a1", "memory");
}

/*
 * iocs_out232c — output one character via _OUT232C (IOCS 0x35)
 *
 * d0 = 0x35 (function code)
 * d1 = character (low byte)
 * Clobbers: d0, d2, a0, a1 (per X68000 IOCS calling convention)
 * In XEiJ, RS-232C serial output appears on stdio.
 */
static inline void iocs_out232c(char c)
{
    register int32_t d0 asm("d0") = 0x35;
    register int32_t d1 asm("d1") = (unsigned char)c;
    asm volatile("trap #15"
                 : "+r"(d0)
                 : "r"(d1)
                 : "d2", "a0", "a1", "memory");
}

/*
 * iocs_b_getc — blocking character input via _B_GETC (IOCS 0x21)
 *
 * d0 = 0x21 (function code)
 * Returns character in d0.l (low byte is the ASCII code).
 */
static inline int iocs_b_getc(void)
{
    register int32_t d0 asm("d0") = 0x21;
    asm volatile("trap #15"
                 : "+r"(d0)
                 : : "d1", "d2", "a0", "a1", "memory");
    return d0 & 0xFF;
}

/*
 * iocs_b_keysns — non-blocking keyboard sense via _B_KEYSNS (IOCS 0x1C)
 *
 * d0 = 0x1C (function code)
 * Returns 0 if no key is available, non-zero if a key is waiting.
 */
static inline int iocs_b_keysns(void)
{
    register int32_t d0 asm("d0") = 0x1C;
    asm volatile("trap #15"
                 : "+r"(d0)
                 : : "d1", "d2", "a0", "a1", "memory");
    return d0;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void uart_init_console(void)
{
    /* IOCS console is initialized by the IPL ROM before stage1 runs.
     * _SET232C (0x32) intentionally not called here: it blocks waiting
     * for CTS on X68000 hardware/XEiJ, hanging the kernel before any output. */
}

void uart_putc(char c)
{
    iocs_b_putc(c);
    iocs_out232c(c);
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            iocs_b_putc('\r');
            iocs_out232c('\r');
        }
        iocs_b_putc(*s);
        iocs_out232c(*s);
        s++;
    }
}

void uart_flush(void)
{
    /* IOCS _B_PUTC is synchronous — no TX buffer to flush */
}

void uart_reinit_133mhz(void)
{
    /* Not applicable — X68000 clock is fixed; IOCS needs no reinit */
}

void uart_init_irq(void)
{
    /* Phase X-2: enable MFP UART RX interrupt for non-blocking input.
     * For now, polling via _B_KEYSNS is used. */
}

int uart_getc(void)
{
    if (!iocs_b_keysns())
        return -1;
    return iocs_b_getc();
}

int uart_rx_avail(void)
{
    return iocs_b_keysns() ? 1 : 0;
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
