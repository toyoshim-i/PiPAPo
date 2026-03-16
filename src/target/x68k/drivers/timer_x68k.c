/*
 * timer_x68k.c — MFP Timer-C driver for X68000 (Phase X-2)
 *
 * Configures the MC68901 MFP Timer-C to generate a 100 Hz interrupt and
 * wires it to the preemptive scheduler.
 *
 * X68000 MFP (MC68901) at 0xE88000:
 *   Registers are 8-bit, accessed at odd byte addresses with stride 2:
 *     address = 0xE88001 + register_index * 2
 *
 *   Timer-C clock:   4 MHz / prescaler(200) / count(200) = 100 Hz
 *   Timer-C vector:  MFP VR base(0x40) + source(5) = 0x45 = vector 69
 *   CPU IRQ level:   MFP IRQ\ → Level 2 (vectored, not autovectored)
 *
 * MFP operates in AEI (Automatic End of Interrupt) mode because the IPL
 * ROM sets VR bit 3 = 0.  The interrupt pending bit is cleared automatically
 * during the CPU's IACK cycle; no explicit acknowledgment is needed in the
 * ISR.
 */

#include "proc/sched.h"
#include <stdint.h>

/* ── MFP register map ───────────────────────────────────────────────────── */

/* MC68901 registers: byte-wide, at 0xE88001 + index*2 on X68000 */
#define MFP_BASE   0xE88001u
#define MFP_REG(n) (*(volatile uint8_t *)(MFP_BASE + (uint32_t)(n) * 2u))

#define MFP_IERB   MFP_REG(4)    /* 0xE88009  Interrupt Enable  B */
#define MFP_IPRB   MFP_REG(6)    /* 0xE8800D  Interrupt Pending B */
#define MFP_IMRB   MFP_REG(10)   /* 0xE88015  Interrupt Mask    B */
#define MFP_TCDCR  MFP_REG(14)   /* 0xE8801D  Timer C/D Control  */
#define MFP_TCDR   MFP_REG(17)   /* 0xE88023  Timer C Data       */

/* Timer-C bit in IER/IPR/IMR-B (source 5 = bit 5) */
#define TC_BIT     0x20u

/* TCDCR bits 6-4: Timer-C prescaler.  7 = divide-by-200. */
#define TC_DIV200  0x70u
#define TC_MASK    0x70u

/* Timer-C count register value: 4 MHz / 200 / 200 = 100 Hz */
#define TC_COUNT   200u

/* Timer-C interrupt vector: VR(0x40) + source(5) = 0x45 = vector 69 */
#define TC_VECTOR  69u

/* m68k_timer_isr defined in src/arch/m68k/switch.S */
extern void m68k_timer_isr(void);

/* ── Public API ─────────────────────────────────────────────────────────── */

void timer_init(void)
{
    /* 1. Stop Timer-C (prescaler = 0), preserve Timer-D bits */
    MFP_TCDCR = MFP_TCDCR & (uint8_t)~TC_MASK;

    /* 2. Load count register (reload value for continuous mode) */
    MFP_TCDR = (uint8_t)TC_COUNT;

    /* 3. Clear any stale pending interrupt */
    MFP_IPRB = MFP_IPRB & (uint8_t)~TC_BIT;

    /* 4. Enable Timer-C interrupt source */
    MFP_IERB = MFP_IERB | (uint8_t)TC_BIT;

    /* 5. Install ISR at vector 0x45 (69) in the CPU vector table */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    volatile uint32_t *vt = (volatile uint32_t *)0x0;
    vt[TC_VECTOR] = (uint32_t)(uintptr_t)m68k_timer_isr;
#pragma GCC diagnostic pop

    /* 6. Start Timer-C: prescaler ÷200 (continuous mode) */
    MFP_TCDCR = (MFP_TCDCR & (uint8_t)~TC_MASK) | (uint8_t)TC_DIV200;

    /* 7. Unmask Timer-C interrupt */
    MFP_IMRB = MFP_IMRB | (uint8_t)TC_BIT;
}

/*
 * m68k_timer_handler_c — called from m68k_timer_isr (switch.S)
 *
 * No hardware acknowledgment is needed: MFP AEI mode clears the pending
 * bit automatically during the CPU's IACK cycle.
 */
void m68k_timer_handler_c(void)
{
    sched_timer_tick(0);
}
