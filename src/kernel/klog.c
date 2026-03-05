/*
 * klog.c — Atomic kernel logging (SMP-safe)
 *
 * klogf() acquires SPIN_UART for the entire format + output sequence,
 * ensuring no interleaving from the other core.
 *
 * Supported format specifiers:
 *   %s — const char * string
 *   %u — uint32_t unsigned decimal
 *   %x — uint32_t as "0xXXXXXXXX"
 *   %% — literal '%'
 */

#include "klog.h"
#include <stdarg.h>

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
                uart_puts(va_arg(ap, const char *));
                break;
            case 'u':
                uart_print_dec(va_arg(ap, uint32_t));
                break;
            case 'x':
                uart_print_hex32(va_arg(ap, uint32_t));
                break;
            case '%':
                uart_putc('%');
                break;
            case '\0':
                goto done;
            default:
                uart_putc('%');
                uart_putc(*fmt);
                break;
            }
        } else {
            if (*fmt == '\n')
                uart_putc('\r');
            uart_putc(*fmt);
        }
        fmt++;
    }
done:
    va_end(ap);
    spin_unlock_irqrestore(SPIN_UART, saved);
}
