/*
 * timer_pit.c -- 8253/8254 PIT and 8259A PIC initialization for IBM PC
 *
 * Programs PIT Channel 0 for 100 Hz periodic interrupts (IRQ 0 → INT 08h).
 * Installs the timer ISR into the IVT.
 */

#include "arch/i16/cpu.h"

/* Defined in switch.S */
extern void i16_timer_isr(void);

/* -- IVT manipulation ----------------------------------------------------- */

static void set_ivt(uint8_t vector, void (*handler)(void))
{
  /* IVT at linear 0x00000.  DS=0 in flat model, so near pointers work. */
  uint16_t *ivt = (uint16_t *)0;
  ivt[vector * 2 + 0] = (uint16_t)(uintptr_t)handler;  /* IP */
  ivt[vector * 2 + 1] = 0x0000;                          /* CS = 0 */
}

/* -- Public API ----------------------------------------------------------- */

void timer_init(void)
{
  __asm__ volatile ("cli");

  /* BIOS already initialized the PIC (IRQ 0 → INT 08h).
   * Just unmask IRQ 0 (timer) and mask everything else. */
  outb(PIC1_DATA, 0xFE);

  /* Program PIT Channel 0: mode 3 (square wave), 100 Hz */
  outb(PIT_CMD, 0x36);
  outb(PIT_CH0, PIT_DIV_100HZ & 0xFF);
  outb(PIT_CH0, PIT_DIV_100HZ >> 8);

  /* Install timer ISR at INT 08h */
  set_ivt(0x08, i16_timer_isr);

  /* Interrupts will be enabled by the caller (kmain) */
}
