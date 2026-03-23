/*
 * cpu.h -- i8086 port I/O and hardware definitions
 *
 * Provides inline port I/O functions and register addresses for the
 * IBM PC/XT hardware: 8259A PIC, 8253/8254 PIT, 8250 UART (COM1).
 */

#ifndef PPAP_ARCH_I16_CPU_H
#define PPAP_ARCH_I16_CPU_H

#include <stdint.h>

/* -- Port I/O -------------------------------------------------------------- */

static inline void outb(uint16_t port, uint8_t val)
{
  __asm__ volatile ("outb %%al, %%dx" :: "a"(val), "d"(port));
}

static inline uint8_t inb(uint16_t port)
{
  uint8_t val;
  __asm__ volatile ("inb %%dx, %%al" : "=a"(val) : "d"(port));
  return val;
}

static inline void io_wait(void)
{
  outb(0x80, 0);  /* Write to unused port for ~1 us delay */
}

/* -- 8259A PIC ------------------------------------------------------------- */

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21

#define PIC_EOI    0x20  /* Non-specific End of Interrupt */

/* -- 8253/8254 PIT --------------------------------------------------------- */

#define PIT_CH0    0x40  /* Channel 0 data port */
#define PIT_CMD    0x43  /* Mode/command register */

/* PIT input clock: 1,193,182 Hz */
#define PIT_FREQ   1193182u
/* Divisor for 100 Hz: 1193182 / 100 = 11932 (0x2E9C) */
#define PIT_DIV_100HZ 11932u

/* -- 8250/16550 UART (COM1) ------------------------------------------------ */

#define COM1_BASE  0x3F8
#define COM1_RBR   (COM1_BASE + 0)  /* Receive Buffer Register (read) */
#define COM1_THR   (COM1_BASE + 0)  /* Transmit Holding Register (write) */
#define COM1_DLL   (COM1_BASE + 0)  /* Divisor Latch Low (DLAB=1) */
#define COM1_DLH   (COM1_BASE + 1)  /* Divisor Latch High (DLAB=1) */
#define COM1_IER   (COM1_BASE + 1)  /* Interrupt Enable Register */
#define COM1_FCR   (COM1_BASE + 2)  /* FIFO Control Register (write) */
#define COM1_LCR   (COM1_BASE + 3)  /* Line Control Register */
#define COM1_MCR   (COM1_BASE + 4)  /* Modem Control Register */
#define COM1_LSR   (COM1_BASE + 5)  /* Line Status Register */

#define LSR_DR     (1u << 0)  /* Data Ready */
#define LSR_THRE   (1u << 5)  /* Transmit Holding Register Empty */

#define LCR_DLAB   (1u << 7)  /* Divisor Latch Access Bit */
#define LCR_8N1    0x03       /* 8 data bits, no parity, 1 stop bit */

/* Baud rate divisor: 115200 / baud.  Base clock = 1.8432 MHz.
 * 115200 baud → divisor 1, 9600 baud → divisor 12 */
#define UART_DIV_9600   12
#define UART_DIV_115200 1

#endif /* PPAP_ARCH_I16_CPU_H */
