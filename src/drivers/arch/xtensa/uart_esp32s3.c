/*
 * uart_esp32s3.c — UART driver for ESP32-S3 via ESP-IDF ROM functions
 *
 * ESP-IDF routes console output through either UART0 or USB Serial JTAG
 * depending on the chip and sdkconfig.  Rather than hitting UART0 registers
 * directly (which won't work on USB-JTAG consoles like the CardComputer),
 * we use the ESP-IDF ROM putchar/getchar functions that go through the
 * correct console channel.
 */

#include "drivers/uart.h"

/* ESP-IDF ROM console functions — available without pulling in full
 * ESP-IDF UART driver.  Declared in esp_rom/include/esp_rom_uart.h
 * but we declare them here to avoid header dependency issues. */
extern void esp_rom_uart_putc(char c);
extern int esp_rom_uart_rx_one_char(unsigned char *c);

/* One-character peek buffer.  esp_rom_uart_rx_one_char() is destructive
 * (no way to put a character back), so uart_rx_avail() reads one char
 * into this buffer and uart_getc() drains it first. */
static int peek_valid;
static unsigned char peek_char;

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
    if (esp_rom_uart_rx_one_char(&c) == 0)
        return (int)c;
    return -1;
}

int uart_rx_avail(void)
{
    if (peek_valid)
        return 1;
    unsigned char c;
    if (esp_rom_uart_rx_one_char(&c) == 0) {
        peek_char = c;
        peek_valid = 1;
        return 1;
    }
    return 0;
}
