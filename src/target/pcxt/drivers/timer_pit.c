/*
 * timer_pit.c -- 8253/8254 PIT and 8259A PIC initialization for IBM PC
 *
 * Programs PIT Channel 0 for 100 Hz periodic interrupts (IRQ 0 → INT 08h).
 * Installs the timer ISR into the IVT, chaining to the original BIOS
 * handler so that BIOS floppy motor timeout and other services still work.
 */

#include "arch/i16/cpu.h"
#include "common/seg.h"

/* Defined in switch.S */
extern void i16_timer_isr(void);
/* Defined in trap.S */
extern void i16_syscall_isr(void);

/* Saved BIOS INT 08h handler — called from i16_bios_timer_chain (below).
 * Written by timer_init() before installing our ISR. */
static uint16_t bios_int08_ip;
static uint16_t bios_int08_cs;

/* Chain to the original BIOS INT 08h handler.
 * Called from switch.S after our timer processing, BEFORE sending EOI.
 * The BIOS handler sends its own EOI, so we must NOT send one ourselves. */
void i16_bios_timer_chain(void)
{
  /* Far call to the saved BIOS handler via inline asm.
   * The BIOS handler expects to be called via INT (or far call with
   * flags on stack).  We simulate this with pushf + lcall. */
  uint16_t ip = bios_int08_ip;
  uint16_t cs = bios_int08_cs;
  __asm__ volatile (
    "pushf\n\t"
    "lcall *%%ss:%0"
    : : "m" (ip), "m" (cs)
    : "ax", "bx", "cx", "dx", "si", "di", "memory", "cc"
  );
}

/* -- IVT manipulation ----------------------------------------------------- */

static void set_ivt(uint8_t vector, void (*handler)(void), uint16_t cs)
{
  uint16_t *ivt = (uint16_t *)0;
  ivt[vector * 2 + 0] = (uint16_t)(uintptr_t)handler;
  ivt[vector * 2 + 1] = cs;
}

/* -- Public API ----------------------------------------------------------- */

void timer_init(void)
{
  __asm__ volatile ("cli");

  /* Save the original BIOS INT 08h handler before overwriting it.
   * We chain to it from our ISR so floppy motor timeout etc. work. */
  {
    volatile uint16_t *ivt = (volatile uint16_t *)0;
    bios_int08_ip = ivt[0x08 * 2 + 0];
    bios_int08_cs = ivt[0x08 * 2 + 1];
  }

  /* BIOS already initialized the PIC (IRQ 0 → INT 08h).
   * Just unmask IRQ 0 (timer) and mask everything else. */
  outb(PIC1_DATA, 0xFE);

  /* Program PIT Channel 0: mode 3 (square wave), 100 Hz */
  outb(PIT_CMD, 0x36);
  outb(PIT_CH0, PIT_DIV_100HZ & 0xFF);
  outb(PIT_CH0, PIT_DIV_100HZ >> 8);

  uint16_t core_cs = seg_get(MOD_ID_CORE);

  /* Install timer ISR at INT 08h */
  set_ivt(0x08, i16_timer_isr, core_cs);

  /* Install syscall handler at INT 30h */
  set_ivt(0x30, i16_syscall_isr, core_cs);

  /* Interrupts will be enabled by the caller */
}
