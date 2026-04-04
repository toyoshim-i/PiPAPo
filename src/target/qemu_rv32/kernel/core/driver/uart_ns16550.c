/*
 * uart_ns16550.c — NS16550A UART driver for QEMU virt machine
 *
 * QEMU virt: UART0 at 0x10000000 (NS16550A compatible).
 * Polled I/O only (no interrupts) — same model as pico2rv.
 */

#include <stdint.h>
#include "kernel/core/driver/uart.h"

#define UART0_BASE 0x10000000u

#define UART_RBR (*(volatile uint8_t *)(UART0_BASE + 0x00)) /* RX buffer */
#define UART_THR (*(volatile uint8_t *)(UART0_BASE + 0x00)) /* TX holding */
#define UART_IER (*(volatile uint8_t *)(UART0_BASE + 0x01)) /* IRQ enable */
#define UART_FCR (*(volatile uint8_t *)(UART0_BASE + 0x02)) /* FIFO ctrl */
#define UART_LCR (*(volatile uint8_t *)(UART0_BASE + 0x03)) /* Line ctrl */
#define UART_LSR (*(volatile uint8_t *)(UART0_BASE + 0x05)) /* Line status */

#define LSR_DR   (1u << 0) /* Data Ready */
#define LSR_THRE (1u << 5) /* TX Holding Register Empty */

/* ── RX ring buffer ──────────────────────────────────────────────────── */

#define RX_BUF_SIZE 64
static volatile uint8_t rx_buf[RX_BUF_SIZE];
static volatile uint8_t rx_head, rx_tail;

static void rx_drain(void) {
  while (UART_LSR & LSR_DR) {
    uint8_t c = UART_RBR;
    uint8_t next = (rx_head + 1) % RX_BUF_SIZE;
    if (next != rx_tail) {
      rx_buf[rx_head] = c;
      rx_head = next;
    }
  }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void uart_init(void) {
  UART_IER = 0;           /* disable all interrupts */
  UART_FCR = 0x07;        /* enable + reset FIFOs */
  UART_LCR = 0x03;        /* 8N1 */
  rx_head = rx_tail = 0;
}

int uart_putc(char c, void (*notify)(void)) {
  (void)notify;
  while (!(UART_LSR & LSR_THRE))
    ;
  UART_THR = (uint8_t)c;
  return 1; /* success — klog loops while return is 0 */
}

int uart_getc(void) {
  rx_drain();
  if (rx_head == rx_tail) return -1;
  uint8_t c = rx_buf[rx_tail];
  rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
  return c;
}

int uart_rx_avail(void) {
  rx_drain();
  return rx_head != rx_tail;
}
