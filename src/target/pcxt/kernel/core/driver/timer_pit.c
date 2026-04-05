/*
 * timer_pit.c -- 8253/8254 PIT and 8259A PIC initialization for IBM PC
 *
 * Programs PIT Channel 0 for 100 Hz periodic interrupts (IRQ 0 → INT 08h).
 * Installs the timer ISR into the IVT, chaining to the original BIOS
 * handler so that BIOS floppy motor timeout and other services still work.
 */

#include "kernel/common/core/seg.h"
#include "kernel/common/ioregs.h"

/* Defined in switch.S */
extern void i16_timer_isr(void);
/* Defined in trap.S */
extern void i16_syscall_isr(void);

/* Saved BIOS INT 08h handler — written by timer_init() before
 * installing our ISR and used by i16_bios_timer_chain().
 * Must be a contiguous IP:CS pair for lcall *addr. */
static struct {
  uint16_t ip;
  uint16_t cs;
} bios_int08;

/* Chain to the original BIOS INT 08h handler.
 * Called from switch.S after our timer processing, BEFORE sending EOI.
 * The BIOS handler sends its own EOI, so we must NOT send one ourselves. */
/* We currently keep the BIOS PIT rate (18.2 Hz), so the BIOS INT 08h
 * handler must run on every tick.  If/when we re-enable 100 Hz PIT
 * programming, this divisor should be raised accordingly. */
#define BIOS_CHAIN_DIVISOR 1u

void i16_bios_timer_chain(void)
{
  /* Counts our timer ISR ticks since the last BIOS INT 08h chain.
   * With BIOS_CHAIN_DIVISOR=1 we chain every tick and div_count is
   * reset immediately.  If the PIT is later reprogrammed faster than
   * the BIOS rate, this lets us call the BIOS handler only every Nth
   * interrupt while still acknowledging the skipped IRQ0s ourselves. */
  static uint8_t div_count;

  if (++div_count >= BIOS_CHAIN_DIVISOR) {
    div_count = 0;
    /* Chain to BIOS handler — it sends EOI and manages motor timeout.
     * lcall *addr reads IP:CS from consecutive memory words. */
    __asm__ volatile (
      "pushf\n\t"
      "lcall *%%ss:%0"
      : : "m" (bios_int08)
      : "ax", "bx", "cx", "dx", "si", "di", "memory", "cc"
    );
  } else {
    /* Non-chained tick: just send EOI ourselves */
    outb(PIC1_CMD, 0x20);
  }
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
    bios_int08.ip = ivt[0x08 * 2 + 0];
    bios_int08.cs = ivt[0x08 * 2 + 1];
  }

  /* BIOS already initialized the PIC and IRQ masks.  Preserve the
   * BIOS-enabled device IRQs (notably IRQ 6 for floppy completion)
   * and only ensure IRQ 0 is unmasked for the timer. */
  outb(PIC1_DATA, inb(PIC1_DATA) & (uint8_t)~0x01u);

  /* Keep the BIOS PIT rate (18.2 Hz) for now.  Reprogramming to 100 Hz
   * breaks BIOS INT 13h floppy reads on QEMU — the BIOS handler's
   * internal timing is calibrated for the default rate.
   * TODO: reprogram to 100 Hz once we have a native floppy driver
   * that doesn't depend on BIOS INT 13h. */
#if 0
  /* Program PIT Channel 0: mode 3 (square wave), 100 Hz */
  outb(PIT_CMD, 0x36);
  outb(PIT_CH0, PIT_DIV_100HZ & 0xFF);
  outb(PIT_CH0, PIT_DIV_100HZ >> 8);
#endif

  uint16_t core_cs = seg_get(MOD_ID_CORE);

  /* Install timer ISR at INT 08h */
  set_ivt(0x08, i16_timer_isr, core_cs);

  /* Install syscall handler at INT 30h */
  set_ivt(0x30, i16_syscall_isr, core_cs);

  /* Interrupts will be enabled by the caller */
}
