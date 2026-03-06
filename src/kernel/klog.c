/*
 * klog.c — Atomic kernel logging (SMP-safe)
 *
 * klog() and klogf() acquire SPIN_UART for the entire output sequence,
 * ensuring no interleaving from the other core.
 *
 * An optional mirror sink (e.g. fbcon for LCD) can be registered via
 * klog_set_mirror().  When set, output goes to both UART and the mirror.
 *
 * Supported format specifiers:
 *   %s — const char * string
 *   %u — uint32_t unsigned decimal
 *   %x — uint32_t as "0xXXXXXXXX"
 *   %% — literal '%'
 */

#include "klog.h"
#include "spinlock.h"
#include "drivers/uart.h"
#include <stdarg.h>

/* ── Mirror output sink ──────────────────────────────────────────────────── */

static void (*mirror_putc)(char c);
static void (*mirror_flush)(void);

void klog_set_mirror(void (*putc)(char c), void (*flush)(void))
{
    mirror_putc  = putc;
    mirror_flush = flush;
}

/* ── Internal helpers (UART + mirror) ────────────────────────────────────── */

static void klog_putc(char c)
{
    uart_putc(c);
    if (mirror_putc)
        mirror_putc(c);
}

static void klog_puts_raw(const char *s)
{
    /* uart_puts() handles \n → \r\n internally */
    uart_puts(s);
    if (mirror_putc) {
        while (*s)
            mirror_putc(*s++);
    }
}

static void klog_print_hex32(uint32_t v)
{
    klog_putc('0');
    klog_putc('x');
    for (int i = 7; i >= 0; i--) {
        unsigned nibble = (v >> (i * 4)) & 0xFu;
        klog_putc(nibble < 10u ? (char)('0' + nibble)
                               : (char)('a' + nibble - 10u));
    }
}

static void klog_print_dec(uint32_t v)
{
    char buf[10];   /* 2^32 = 4294967296 — at most 10 digits */
    int  i = 0;
    if (v == 0u) { klog_putc('0'); return; }
    while (v > 0u) { buf[i++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (--i >= 0) klog_putc(buf[i]);
}

static inline void klog_finish(void)
{
    if (mirror_flush)
        mirror_flush();
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void klog(const char *msg)
{
    uint32_t s = spin_lock_irqsave(SPIN_UART);
    klog_puts_raw(msg);
    klog_finish();
    spin_unlock_irqrestore(SPIN_UART, s);
}

void klogf(const char *fmt, ...)
{
    uint32_t saved = spin_lock_irqsave(SPIN_UART);

    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
            case 's':
                klog_puts_raw(va_arg(ap, const char *));
                break;
            case 'u':
                klog_print_dec(va_arg(ap, uint32_t));
                break;
            case 'x':
                klog_print_hex32(va_arg(ap, uint32_t));
                break;
            case '%':
                klog_putc('%');
                break;
            case '\0':
                goto done;
            default:
                klog_putc('%');
                klog_putc(*fmt);
                break;
            }
        } else {
            if (*fmt == '\n')
                klog_putc('\r');
            klog_putc(*fmt);
        }
        fmt++;
    }
done:
    va_end(ap);
    klog_finish();
    spin_unlock_irqrestore(SPIN_UART, saved);
}
