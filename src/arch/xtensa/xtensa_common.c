/*
 * xtensa_common.c — Shared Xtensa architecture state
 *
 * Provides:
 *   - Context switch pending flag (checked by timer ISR / yield)
 *   - CCOMPARE0 timer init and ISR
 *   - xtensa_do_switch() — C helper called from switch.S
 *   - Exception handler (minimal, for debugging)
 */

#include <stdint.h>
#include "cpu.h"
#include "../../kernel/proc/proc.h"
#include "../../kernel/proc/sched.h"

/* Context switch pending flag.
 * Set by arch_yield() (via sched_tick or sched_yield).
 * Checked by switch.S / sched_yield to perform the switch.
 * Same pattern as riscv_switch_pending / m68k_switch_pending. */
volatile uint32_t xtensa_switch_pending = 0;

/* Tick counter — incremented by timer ISR. */
volatile uint32_t xtensa_tick_count = 0;

/* ── CCOMPARE0 timer ─────────────────────────────────────────────────────── */

/* Timer ISR — called from ESP-IDF's level-1 interrupt dispatch.
 * Rearms CCOMPARE0 and calls the shared scheduler tick handler. */
static void xtensa_timer_isr(void *arg)
{
    (void)arg;
    uint32_t cmp;
    __asm__ volatile("rsr %0, ccompare0" : "=a"(cmp));
    __asm__ volatile("wsr %0, ccompare0" :: "a"(cmp + XTENSA_TICK_INTERVAL));
    __asm__ volatile("esync");
    xtensa_tick_count++;
    sched_timer_tick(0); /* from_user=0 (no user/kernel split yet) */
}

void xtensa_timer_init(void)
{
    typedef void (*xt_handler)(void *);
    extern xt_handler xt_set_interrupt_handler(int n, xt_handler f, void *arg);

    /* Set CCOMPARE0 for first tick */
    uint32_t cc;
    __asm__ volatile("rsr %0, ccount" : "=a"(cc));
    __asm__ volatile("wsr %0, ccompare0" :: "a"(cc + XTENSA_TICK_INTERVAL));
    __asm__ volatile("esync");

    /* Register the CCOMPARE0 timer ISR */
    xt_set_interrupt_handler(XTENSA_TIMER0_INTNUM, xtensa_timer_isr, (void *)0);

    /* Set INTENABLE to ONLY the CCOMPARE0 bit (bit 6).
     * This replaces whatever FreeRTOS had configured (including bit 12
     * for SYSTIMER).  Do not OR — explicitly set to prevent stray
     * interrupts from firing. */
    __asm__ volatile("wsr %0, intenable" :: "a"(XTENSA_TIMER0_INTMASK));
    __asm__ volatile("rsync");
}

/* ── Context switch helper ───────────────────────────────────────────────── */

/* Called from switch.S (xtensa_do_yield) with the current SP.
 * Saves SP to old PCB, picks next process, returns new SP. */
uint32_t xtensa_do_switch(uint32_t current_sp)
{
    if (current)
        current->sp = current_sp;
    pcb_t *next = sched_next();
    current_core[core_id()] = next;
    return next->sp;
}

/* ── Exception handler ────────────────────────────────────────────────────── */

void xtensa_exception_handler(uint32_t exccause, uint32_t epc1,
                              uint32_t excvaddr)
{
    (void)exccause;
    (void)epc1;
    (void)excvaddr;

    uint32_t dummy;
    __asm__ volatile ("rsil %0, 15" : "=a"(dummy));

    for (;;)
        __asm__ volatile ("waiti 15");
}

/* ── Initial stack frame for new processes ──────────────────────────────── */

uint32_t *arch_build_initial_frame(uint32_t *sp, void (*entry)(void))
{
    sp = (uint32_t *)((uintptr_t)sp & ~0xFu);  /* 16-byte align */
    /* Base save area (16 bytes): retw underflow reads caller's a0-a3 */
    *--sp = 0u;  /* a3 */
    *--sp = 0u;  /* a2 */
    *--sp = 0u;  /* a1 */
    *--sp = 0u;  /* a0 */
    /* Solicited frame (16 bytes) — must match switch.S layout */
    *--sp = 0u;                                             /* padding */
    *--sp = (1u << 18);                                     /* PS: WOE=1 */
    *--sp = ((uint32_t)entry & 0x3FFFFFFFu) | (1u << 30);  /* PC */
    *--sp = 0u;                                             /* exit = 0 */
    return sp;
}
