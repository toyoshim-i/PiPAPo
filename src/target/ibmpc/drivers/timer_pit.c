/*
 * timer_pit.c -- 8253/8254 PIT and 8259A PIC initialization for IBM PC
 *
 * Programs PIT Channel 0 for 100 Hz periodic interrupts (IRQ 0 → INT 08h).
 * Initializes the 8259A PIC and installs the timer ISR into the IVT.
 */

#include "arch/i16/cpu.h"

/* Defined in switch.S */
extern void i16_timer_isr(void);

/* -- IVT manipulation ----------------------------------------------------- */

static void set_ivt(uint8_t vector, void (*handler)(void))
{
  /* IVT is at linear address 0x00000, each entry is 4 bytes: IP then CS.
   * Since DS=0, we can write directly via a near pointer. */
  uint16_t *ivt = (uint16_t *)0;
  ivt[vector * 2 + 0] = (uint16_t)(uintptr_t)handler;  /* IP */
  ivt[vector * 2 + 1] = 0x0000;                          /* CS = 0 */
}

/* -- Public API ----------------------------------------------------------- */

void timer_init(void)
{
  /* Disable interrupts while modifying IVT and hardware */
  __asm__ volatile ("cli");

  /* BIOS already initialized the PIC (IRQ 0 → INT 08h).
   * Just unmask IRQ 0 (timer) and mask everything else. */
  outb(PIC1_DATA, 0xFE);  /* OCW1: unmask bit 0 only */

  /* Program PIT Channel 0: mode 3 (square wave), 100 Hz */
  outb(PIT_CMD, 0x36);                   /* Ch0, lo/hi byte, mode 3 */
  outb(PIT_CH0, PIT_DIV_100HZ & 0xFF);   /* Divisor low byte */
  outb(PIT_CH0, PIT_DIV_100HZ >> 8);     /* Divisor high byte */

  /* Install timer ISR at INT 08h in the IVT */
  set_ivt(0x08, i16_timer_isr);

  /* Interrupts will be enabled by the caller (kmain) */
}
