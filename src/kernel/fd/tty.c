/*
 * tty.c — UART tty driver (struct file_ops implementation)
 *
 * Backed by uart_putc() / uart_getc() from drivers/uart.h.
 * After uart_init_irq() those calls are non-blocking ring-buffer operations;
 * the UART0_IRQ_Handler drains TX and fills RX asynchronously.
 *
 * tty_write: iterates the buffer, expands '\n' → '\r\n' for terminal compat,
 *            and calls uart_putc() for each byte.
 *
 * tty_read:  reads up to n bytes from the RX ring, stopping on '\n'
 *            (line-buffered).  Spins with WFI if no data is available yet
 *            so SysTick can preempt and other threads can run.
 *            On QEMU (no UART RX IRQ) the WFI wakes on the next SysTick.
 *
 * tty_close: no-op — the tty is never really closed.
 */

#include "tty.h"
#include "file.h"
#include "drivers/uart.h"
#include <stddef.h>

/* ── tty file operations ────────────────────────────────────────────────────── */

static long tty_write(struct file *f, const char *buf, size_t n)
{
    (void)f;
    for (size_t i = 0u; i < n; i++) {
        if (buf[i] == '\n')
            uart_putc('\r');    /* expand LF → CR+LF for terminal compatibility */
        uart_putc(buf[i]);
    }
    return (long)n;
}

static long tty_read(struct file *f, char *buf, size_t n)
{
    (void)f;
    size_t i = 0u;
    while (i < n) {
        int c = uart_getc();
        if (c < 0) {
            if (i)
                break;              /* already have bytes — return them */
            __asm__ volatile("wfi"); /* sleep until UART RX IRQ or SysTick */
            continue;
        }
        buf[i++] = (char)c;
        if (c == '\n')
            break;                  /* line-buffered read */
    }
    return (long)i;
}

static int tty_close(struct file *f)
{
    (void)f;
    return 0;   /* tty is never truly closed */
}

/* ── Static file objects ────────────────────────────────────────────────────── */

static const struct file_ops tty_fops = { tty_read, tty_write, tty_close };

struct file tty_stdin  = { &tty_fops, NULL, O_RDONLY, 1u };
struct file tty_stdout = { &tty_fops, NULL, O_WRONLY, 1u };
struct file tty_stderr = { &tty_fops, NULL, O_WRONLY, 1u };
