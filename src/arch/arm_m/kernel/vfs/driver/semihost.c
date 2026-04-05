/*
 * semihost.c — ARM semihosting serial I/O driver
 *
 * Implements the uart.h API using ARM semihosting (bkpt 0xAB on Cortex-M).
 * The debugger (QEMU with -semihosting, or OpenOCD with "arm semihosting
 * enable") intercepts the breakpoint and performs I/O on the host.
 *
 * This is a compile-time alternative to the hardware UART drivers
 * (uart_rpico.c, uart_qemu.c).  Enabled by -DPPAP_SEMIHOST=ON.
 *
 * Semihosting operations used:
 *   SYS_WRITEC (0x03) — write one character (r1 = pointer to char)
 *   SYS_READC  (0x07) — read one character (returns char or -1)
 *
 * Notes:
 *   - Output is synchronous: putc always succeeds (returns 1).
 *   - Input: SYS_READC is non-blocking on QEMU (returns -1 immediately
 *     if no input), but may block on OpenOCD.
 *   - No ISR, no ring buffer, no notify callback needed.
 *   - Without a semihosting-aware debugger, bkpt 0xAB triggers HardFault.
 */

#include "kernel/vfs/uart.h"

/* ── Semihosting call primitive ──────────────────────────────────────────── */

#define SYS_WRITEC 0x03
#define SYS_READC  0x07

static inline int semihost_call(int op, const void *arg)
{
    register int r0 __asm__("r0") = op;
    register const void *r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}


/* ── uart.h API ──────────────────────────────────────────────────────────── */

void uart_init(void)
{
    /* No hardware to initialize — semihosting is always available
     * when a debugger is attached. */
}

int uart_putc(char c, void (*notify)(void))
{
    (void)notify;
    semihost_call(SYS_WRITEC, &c);
    return 1; /* semihosting is synchronous, never full */
}

int uart_getc(void)
{
    int ch = semihost_call(SYS_READC, NULL);
    return (ch < 0) ? -1 : ch;
}

int uart_rx_avail(void)
{
    return 0; /* no interrupt-driven RX; TTY layer polls via getc */
}
