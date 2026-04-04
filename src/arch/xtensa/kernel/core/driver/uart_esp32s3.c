/*
 * uart_esp32s3.c — UART driver for ESP32-S3 via ESP-IDF ROM functions
 *
 * ESP-IDF routes console I/O through either UART0 or USB Serial JTAG
 * depending on the chip and sdkconfig.  We use ROM functions to stay
 * console-channel agnostic.
 *
 * The ROM rx function (esp_rom_uart_rx_one_char) may internally use
 * `syscall 0` for window spilling.  With PS.UM=0 (kernel idle loop),
 * this would route to KernelExceptionVector which doesn't handle
 * EXCCAUSE_SYSCALL → crash.  We temporarily set PS.UM=1 around the
 * call so any internal syscall routes through UserExceptionVector
 * (which handles syscall 0 correctly).
 */

#include <stdint.h>
#include "core/driver/uart.h"

/* ESP-IDF ROM console functions */
extern void esp_rom_uart_putc(char c);
extern int esp_rom_uart_rx_one_char(unsigned char *c);

/* One-character peek buffer.  ROM rx is destructive (no way to put a
 * character back), so uart_rx_avail() reads one char into this buffer
 * and uart_getc() drains it first. */
static int peek_valid;
static unsigned char peek_char;

/* Call ROM rx with PS.UM=1 so any internal `syscall 0` (window spill)
 * routes through UserExceptionVector instead of KernelExceptionVector. */
static inline int rom_rx_safe(unsigned char *c)
{
    uint32_t ps;
    __asm__ volatile("rsr %0, ps" : "=a"(ps));
    __asm__ volatile("wsr %0, ps; rsync" :: "a"(ps | (1u << 5)));
    int rc = esp_rom_uart_rx_one_char(c);
    __asm__ volatile("wsr %0, ps; rsync" :: "a"(ps));
    return rc;
}

void uart_init(void)
{
    /* ESP-IDF already configured the console channel. */
}

int uart_putc(char c, void (*notify)(void))
{
    (void)notify;
    esp_rom_uart_putc(c);
    return 1;
}

int uart_getc(void)
{
    if (peek_valid) {
        peek_valid = 0;
        return (int)peek_char;
    }
    unsigned char c;
    if (rom_rx_safe(&c) == 0)
        return (int)c;
    return -1;
}

int uart_rx_avail(void)
{
    if (peek_valid)
        return 1;
    unsigned char c;
    if (rom_rx_safe(&c) == 0) {
        peek_char = c;
        peek_valid = 1;
        return 1;
    }
    return 0;
}
