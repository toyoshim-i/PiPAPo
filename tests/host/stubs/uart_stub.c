/*
 * uart_stub.c — Host-side stubs for the UART driver
 *
 * Replaces src/drivers/uart.c when building tests on the host.
 * uart_puts / uart_putc / uart_print_hex32 / uart_print_dec forward to
 * printf so test output from mm_init() (boot memory map) is still visible.
 * uart_flush / uart_getc are no-ops.
 *
 * Also provides the linker symbols that page.c references:
 *   __bss_end   — first byte after .bss
 *   __stack_top — top of the initial kernel stack
 * On the host these are just two static bytes; page.c uses only &__bss_end
 * and &__stack_top (their addresses), not the values stored there.
 */

#include <stdio.h>
#include <stdint.h>

/* ── Linker symbol stubs ─────────────────────────────────────────────────── */

char __bss_end   = 0;
char __stack_top = 0;

/* ── UART stubs ──────────────────────────────────────────────────────────── */

void uart_puts(const char *s)        { fputs(s, stdout); }
void uart_putc(char c)               { putchar((unsigned char)c); }
void uart_flush(void)                { fflush(stdout); }
int  uart_getc(void)                 { return -1; }

void uart_print_hex32(uint32_t v)    { printf("0x%08x", v); }
void uart_print_dec(uint32_t v)      { printf("%u", v); }

/* uart_init_console / uart_init_irq / uart_reinit_133mhz — not needed in
 * unit tests; provided as no-ops to satisfy any transitive link requirements */
void uart_init_console(void)         {}
void uart_init_irq(void)             {}
void uart_reinit_133mhz(void)        {}
