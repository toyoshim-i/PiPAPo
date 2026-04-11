/*
 * uart_com.c -- COM1 (8250/16550) UART driver for IBM PC
 *
 * Polled I/O only for P-1.  IRQ-driven TX/RX will be added in P-2.
 * Register layout matches the NS16550 used in QEMU virt.
 */

#include "kernel/common/ioregs.h"
#include "kernel/vfs/driver/uart.h"

static uint8_t uart_connected = 1u;

void uart_init(void)
{
  /* Disable all interrupts */
  outb(COM1_IER, 0x00);

  /* Set baud rate: 9600 bps (divisor = 12) */
  outb(COM1_LCR, LCR_DLAB);         /* Enable DLAB */
  outb(COM1_DLL, UART_DIV_9600);    /* Divisor low byte */
  outb(COM1_DLH, 0x00);             /* Divisor high byte */

  /* 8N1, clear DLAB */
  outb(COM1_LCR, LCR_8N1);

  /* Enable and reset FIFOs (16550 only; harmless on 8250) */
  outb(COM1_FCR, 0x07);  /* Enable FIFO, clear RX/TX, trigger=1 */

  /* DTR + RTS + OUT2 (OUT2 enables IRQs on PC hardware) */
  outb(COM1_MCR, 0x0B);
  uart_connected = 1u;
}

int uart_putc(char c, void (*notify)(void))
{
  (void)notify;
  uint16_t timeout;

  if (!uart_connected) return 1;

  /* If COM1 stops responding, mark it disconnected and drop TX until
   * receive activity proves a peer is back. */
  for (timeout = 0xFFFFu; timeout != 0u; timeout--) {
    if (inb(COM1_LSR) & LSR_THRE) {
      outb(COM1_THR, c);
      return 1;
    }
  }
  uart_connected = 0u;
  return 1;
}

int uart_getc(void)
{
  if (inb(COM1_LSR) & LSR_DR) {
    uart_connected = 1u;
    return inb(COM1_RBR);
  }
  return -1;
}

int uart_rx_avail(void)
{
  if (inb(COM1_LSR) & LSR_DR) {
    uart_connected = 1u;
    return 1;
  }
  return 0;
}
