/*
 * sched.c — Round-robin preemptive scheduler
 *
 * SysTick fires every SYSTICK_RELOAD+1 CPU cycles and calls sched_tick().
 * sched_tick() triggers PendSV when the current process's time-slice
 * expires.  PendSV_Handler (switch.S) calls sched_next() and swaps context.
 *
 * Priority setup:
 *   - PendSV must be the LOWEST priority exception (priority byte = 0xFF)
 *     so that it never preempts a real interrupt handler (e.g. UART IRQ).
 *     PendSV fires only when Thread mode would regain the CPU.
 *   - SysTick can be any priority higher than PendSV; we leave it at
 *     the reset default (0x00 = highest) for Phase 1.
 */

#include "sched.h"    /* includes proc.h via sched.h */
#include "../mm/page.h"    /* PAGE_SIZE */
#include <stdint.h>

/* ── Cortex-M system registers ─────────────────────────────────────────────── */

/* SysTick — ARM standard (Cortex-M0+ §B3.3) */
#define SYST_CSR    (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR    (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR    (*(volatile uint32_t *)0xE000E018u)

/* Interrupt Control and State Register — bit 28 = PENDSVSET */
#define SCB_ICSR    (*(volatile uint32_t *)0xE000ED04u)
#define PENDSVSET   (1u << 28)

/* System Handler Priority Register 3 — PendSV priority at [23:16] */
#define SCB_SHPR3   (*(volatile uint32_t *)0xE000ED20u)

/* ── Scheduler ─────────────────────────────────────────────────────────────── */

pcb_t *sched_next(void)
{
    uint32_t idx = (uint32_t)(current - proc_table);   /* slot of current */

    for (uint32_t i = 1u; i < PROC_MAX; i++) {
        uint32_t next = (idx + i) % PROC_MAX;
        if (proc_table[next].state == PROC_RUNNABLE)
            return &proc_table[next];
    }
    return current;   /* no other runnable process — keep running */
}

void sched_tick(void)
{
    if (!current)
        return;

    if (--current->ticks_remaining == 0u) {
        current->ticks_remaining = PROC_DEFAULT_TICKS;
        SCB_ICSR |= PENDSVSET;   /* trigger PendSV (runs after SysTick exits) */
    }
    /* Phase 1 Step 9+: wake sleeping processes based on sleep_until */
}

/* ── SysTick exception handler ─────────────────────────────────────────────── */

static volatile uint32_t tick_count = 0u;

void SysTick_Handler(void)
{
    tick_count++;
    sched_tick();
}

/* ── Scheduler startup ─────────────────────────────────────────────────────── */

void sched_start(void)
{
    /* Set PendSV to lowest priority (0xFF) so it never preempts real IRQs.
     * SHPR3[23:16] is the PendSV priority byte on Cortex-M0+. */
    SCB_SHPR3 = (SCB_SHPR3 & ~(0xFFu << 16)) | (0xFFu << 16);

    /*
     * Switch Thread mode from MSP to PSP using Thread 0's dedicated stack.
     *
     * IMPORTANT: proc_table[0].stack_page MUST be allocated before calling
     * sched_start().  We set PSP to the TOP of that 4 KB page so that Thread
     * mode has a stack region completely separate from the MSP exception stack.
     *
     * Why not "mrs r0, msp; msr psp, r0"?
     *   Copying MSP → PSP makes both stacks share the same memory.  When
     *   PendSV_Handler runs (on MSP) and does "push {r2, lr}", it writes into
     *   the region just below the current MSP, which is exactly where Thread 0's
     *   exception frame was pushed by hardware — corrupting the saved PC/XPSR.
     *
     * By pointing PSP at a fresh page we ensure:
     *   • Thread 0 PSP stack lives entirely in [stack_page, stack_page+4KB)
     *   • MSP exception stack lives in the kernel .stack region
     *   • No overlap, no corruption
     *
     * ISB ensures the subsequent instructions see the new CONTROL setting.
     */
    uint32_t psp_top = (uint32_t)(uintptr_t)proc_table[0].stack_page + PAGE_SIZE;
    __asm__ volatile(
        "msr  psp, %0      \n"   /* PSP = top of Thread 0's stack page */
        "movs r0, #2       \n"   /* CONTROL.SPSEL = 1 */
        "msr  control, r0  \n"
        "isb               \n"
        :: "r"(psp_top) : "r0"
    );

    /* Configure SysTick: reload value, clear current count, start. */
    SYST_RVR = SYSTICK_RELOAD;
    SYST_CVR = 0u;
    SYST_CSR = 0x7u;   /* ENABLE | TICKINT | CLKSOURCE (processor clock) */

    /* Enable interrupts — scheduler is now live. */
    __asm__ volatile("cpsie i");
}

/* ── Cooperative yield ─────────────────────────────────────────────────────── */

void sched_yield(void)
{
    SCB_ICSR |= PENDSVSET;   /* pend PendSV; fires at next instruction boundary */
}

