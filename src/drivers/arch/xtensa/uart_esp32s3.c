/*
 * uart_esp32s3.c — UART driver for ESP32-S3 via ESP-IDF ROM functions
 *
 * ESP-IDF routes console output through either UART0 or USB Serial JTAG
 * depending on the chip and sdkconfig.  Rather than hitting UART0 registers
 * directly (which won't work on USB-JTAG consoles like the CardComputer),
 * we use the ESP-IDF ROM putchar/getchar functions that go through the
 * correct console channel.
 *
 * CC-2+ can switch to a direct driver once the console path is known.
 */

#include "drivers/uart.h"

/* ESP-IDF ROM console functions — available without pulling in full
 * ESP-IDF UART driver.  Declared in esp_rom/include/esp_rom_uart.h
 * but we declare them here to avoid header dependency issues. */
extern void esp_rom_uart_putc(char c);
extern int esp_rom_uart_rx_one_char(unsigned char *c);

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
    unsigned char c;
    if (esp_rom_uart_rx_one_char(&c) == 0)
        return (int)c;
    return -1;
}

int uart_rx_avail(void)
{
    /* ROM doesn't provide a non-blocking peek.
     * Return 0 — polling callers will retry. */
    return 0;
}
