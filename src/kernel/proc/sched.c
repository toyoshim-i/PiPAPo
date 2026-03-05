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
#include "../spinlock.h"   /* SPIN_PROC */
#include "hw/cortex_m0plus.h"
#include <stddef.h>
#include <stdint.h>

/* ── Tick counter ───────────────────────────────────────────────────────────── */

/* Incremented by SysTick_Handler every tick.  Declared here (before sched_tick
 * and sched_sleep) so both functions can access it in the same translation unit. */
static volatile uint32_t tick_count = 0u;

/* ── Per-core CPU jiffy counters (for /proc/stat) ────────────────────────── */
uint32_t cpu_user_ticks[2]   = {0, 0};
uint32_t cpu_system_ticks[2] = {0, 0};
uint32_t cpu_idle_ticks[2]   = {0, 0};

uint32_t sched_get_ticks(void)
{
    return tick_count;
}

/* ── Scheduler ─────────────────────────────────────────────────────────────── */

pcb_t *sched_next(void)
{
    uint32_t saved = spin_lock_irqsave(SPIN_PROC);
    uint32_t idx = (uint32_t)(current - proc_table);   /* slot of current */

    pcb_t *result = current;   /* default: keep running */
    for (uint32_t i = 1u; i < PROC_MAX; i++) {
        uint32_t next = (idx + i) % PROC_MAX;
        if (proc_table[next].state == PROC_RUNNABLE
                && proc_table[next].running_on_core < 0) {
            result = &proc_table[next];
            break;
        }
    }

    /* Track which core is running which process. */
    if (result != current) {
        current->running_on_core = -1;
        result->running_on_core = (int8_t)core_id();
    }

    spin_unlock_irqrestore(SPIN_PROC, saved);
    return result;
}

void sched_tick(void)
{
    /* Only Core 0 handles sleep/timeout wakeups (avoids double-waking).
     * Comparison uses signed subtraction to handle tick_count wrap-around:
     *   (int32_t)(tick_count - sleep_until) >= 0  is true when
     *   tick_count >= sleep_until even after the uint32_t counter wraps. */
    if (core_id() == 0) {
        for (uint32_t i = 0u; i < PROC_MAX; i++) {
            pcb_t *p = &proc_table[i];
            if (p->state == PROC_SLEEPING
                    && (int32_t)(tick_count - p->sleep_until) >= 0)
                p->state = PROC_RUNNABLE;
            /* PROC_BLOCKED + sleep_until: poll/select timeout.
             * Wake the process so svc_restart re-checks the condition. */
            if (p->state == PROC_BLOCKED && p->sleep_until != 0
                    && (int32_t)(tick_count - p->sleep_until) >= 0) {
                p->state = PROC_RUNNABLE;
                p->wait_channel = NULL;
                p->sleep_until = 0;
            }
        }
    }

    /* Per-core: decrement time slice and pend PendSV when expired */
    if (!current)
        return;

    if (--current->ticks_remaining == 0u) {
        current->ticks_remaining = PROC_DEFAULT_TICKS;
        SCB_ICSR |= PENDSVSET;   /* trigger PendSV (runs after SysTick exits) */
    }
}

/* ── SysTick exception handler ─────────────────────────────────────────────── */

/* CPU tick accounting needs the EXC_RETURN value that the hardware places in
 * LR on exception entry.  A normal C function's prologue clobbers LR, so we
 * use a naked wrapper to capture it and pass it as the first argument.
 *
 * EXC_RETURN bit 3:  1 = return to Thread mode (user),
 *                    0 = return to Handler mode (kernel).
 *
 * Note: ICSR.RETTOBASE (bit 11) is RAZ on ARMv6-M / Cortex-M0+, so we
 * cannot use it for user-vs-kernel distinction. */

__attribute__((used)) static void SysTick_Handler_c(uint32_t exc_return);

__attribute__((naked)) void SysTick_Handler(void)
{
    __asm__ volatile(
        "push {r0, lr}\n"  /* 8-byte aligned; save EXC_RETURN for return */
        "mov  r0, lr\n"    /* pass EXC_RETURN as first argument */
        "bl   SysTick_Handler_c\n"
        "pop  {r0, pc}\n"  /* pop EXC_RETURN into PC → exception return */
    );
}

static void SysTick_Handler_c(uint32_t exc_return)
{
    /* Only Core 0 maintains the global tick counter */
    if (core_id() == 0)
        tick_count++;

    uint32_t cid = core_id();
    if (current && current->state == PROC_RUNNABLE && !current->is_idle) {
        if (exc_return & (1u << 3)) {
            current->utime++;
            cpu_user_ticks[cid]++;
        } else {
            current->stime++;
            cpu_system_ticks[cid]++;
        }
    } else {
        cpu_idle_ticks[cid]++;
    }

    sched_tick();
}

/* ── Scheduler startup ─────────────────────────────────────────────────────── */

void sched_start(void)
{
    /* Set PendSV to lowest priority (0xFF) so it never preempts real IRQs.
     * SHPR3[23:16] is the PendSV priority byte on Cortex-M0+. */
    SCB_SHPR3 = (SCB_SHPR3 & ~PENDSV_PRIO_MASK) | PENDSV_PRIO_LOWEST;

    /* Lower SVCall priority (0x80) so hardware interrupts (SysTick, UART)
     * can preempt the SVC handler.  Without this, WFI inside blocking
     * syscalls (e.g. tty_read) would never wake — no interrupt can preempt
     * a handler at the default priority 0x00. */
    SCB_SHPR2 = (SCB_SHPR2 & ~SVCALL_PRIO_MASK) | (0x80u << SVCALL_PRIO_SHIFT);

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
    SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;

    /* Enable interrupts — scheduler is now live. */
    __asm__ volatile("cpsie i");
}

/* ── Cooperative yield ─────────────────────────────────────────────────────── */

void sched_yield(void)
{
    SCB_ICSR |= PENDSVSET;   /* pend PendSV; fires at next instruction boundary */
}

/* ── Channel-based wakeup ──────────────────────────────────────────────────── */

void sched_wakeup(void *channel)
{
    uint32_t saved = spin_lock_irqsave(SPIN_PROC);
    int woke = 0;
    for (uint32_t i = 0u; i < PROC_MAX; i++) {
        pcb_t *p = &proc_table[i];
        if (p->state == PROC_BLOCKED && p->wait_channel == channel) {
            p->state = PROC_RUNNABLE;
            p->wait_channel = NULL;
            woke = 1;
        }
    }
    spin_unlock_irqrestore(SPIN_PROC, saved);
    /* Trigger context switch so woken process runs promptly.
     * PendSV has lowest priority — fires after the current ISR returns. */
    if (woke)
        SCB_ICSR |= PENDSVSET;
}

/* ── Sleep ──────────────────────────────────────────────────────────────────── */

void sched_sleep(uint32_t ticks)
{
    current->sleep_until    = tick_count + ticks;
    current->state          = PROC_SLEEPING;
    SCB_ICSR |= PENDSVSET;  /* yield CPU; PendSV fires after caller returns */
    /* Execution resumes here after sched_tick() marks us RUNNABLE again
     * and PendSV restores our context. */
}

