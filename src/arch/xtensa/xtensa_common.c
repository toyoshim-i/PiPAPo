/*
 * xtensa_common.c — Shared Xtensa architecture state
 *
 * Provides:
 *   - Context switch pending flag (checked by timer ISR / yield)
 *   - CCOMPARE0 timer init and ISR
 *   - xtensa_do_switch() — C helper called from switch.S
 *   - Syscall dispatch (via ESP-IDF exception table)
 *   - Exception/fault handler (crash reporting)
 */

#include <stdint.h>
#include "cpu.h"
#include "../../kernel/proc/proc.h"
#include "../../kernel/proc/sched.h"
#include "../../kernel/syscall/syscall.h"
#include "../../kernel/klog.h"

/* ESP-IDF exception frame and handler registration API */
#include "xtensa_api.h"

/* Context switch pending flag.
 * Set by arch_yield() (via sched_tick or sched_yield).
 * Checked by switch.S / sched_yield to perform the switch.
 * Same pattern as riscv_switch_pending / m68k_switch_pending. */
volatile uint32_t xtensa_switch_pending = 0;

/* Timer ready flag — set by xtensa_timer_init().
 * arch_preempt_enable() checks this to avoid enabling the timer interrupt
 * before the ISR is registered. */
volatile uint32_t xtensa_timer_ready = 0;

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

    xtensa_timer_ready = 1;
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

/* ── Syscall handler ──────────────────────────────────────────────────────── */

/* Called by ESP-IDF's exception dispatch when EXCCAUSE == SYSCALL (1).
 * The XtExcFrame has all registers saved; we extract syscall args from
 * a2-a7 and call the shared syscall_dispatch(). */
static void xtensa_syscall_handler(XtExcFrame *frame)
{
    /* Advance PC past the 3-byte SYSCALL instruction */
    frame->pc += 3;

    /* syscall_dispatch(frame, nr, a4, a5):
     *   frame[0..3] = a2,a3,a4,a5 (contiguous in XtExcFrame)
     *   nr          = a7 (syscall number)
     *   a4, a5      = 5th/6th args */
    syscall_dispatch((uint32_t *)&frame->a2, (uint32_t)frame->a7,
                     (uint32_t)frame->a4, (uint32_t)frame->a5);

    /* exec_pending: execve built a new trap frame at current->sp.
     * TODO (CC-4): reload SP from PCB when user processes exist. */
    if (exec_pending[0]) {
        exec_pending[0] = 0;
    }

    /* svc_restart: blocking syscall needs re-execution */
    if (svc_restart[0]) {
        svc_restart[0] = 0;
        frame->pc -= 3;  /* rewind to SYSCALL instruction */
        frame->a2 = svc_saved_a0[0];
    }

    /* Context switch: timer may have fired during syscall */
    if (xtensa_switch_pending) {
        xtensa_switch_pending = 0;
        /* Cooperative switch — will be picked up by idle loop.
         * True preemptive switch deferred to CC-4. */
    }
}

/* ── Fault handler ───────────────────────────────────────────────────────── */

/* Called for all unhandled exceptions (illegal insn, load/store error, etc.).
 * Prints crash info for debugging, kills user processes, halts on kernel
 * faults. */
static void xtensa_fault_handler(XtExcFrame *frame)
{
    klogf("EXCEPTION: cause=%u pc=%x vaddr=%x\n",
          (uint32_t)frame->exccause, (uint32_t)frame->pc,
          (uint32_t)frame->excvaddr);
    klogf("  a0=%x a2=%x a3=%x a7=%x\n",
          (uint32_t)frame->a0, (uint32_t)frame->a2,
          (uint32_t)frame->a3, (uint32_t)frame->a7);

    if (current && !current->is_idle) {
        klogf("  pid=%u comm=%s — killed\n", current->pid, current->comm);
        current->state = PROC_ZOMBIE;
        current->exit_status = 128 + 11; /* SIGSEGV */
        arch_yield();
        return;  /* ESP-IDF restores context; scheduler will pick another */
    }

    /* Kernel fault: halt */
    klog("PANIC: kernel exception — halting\n");
    for (;;)
        __asm__ volatile ("waiti 15");
}

/* ── Trap initialization ─────────────────────────────────────────────────── */

void xtensa_trap_init(void)
{
    /* Install SYSCALL handler */
    xt_set_exception_handler(EXCCAUSE_SYSCALL, xtensa_syscall_handler);

    /* Install fault handler for all other exception causes */
    for (int i = 0; i < 30; i++) {
        if (i == (int)EXCCAUSE_SYSCALL ||
            i == (int)EXCCAUSE_LEVEL1_INT ||
            i == (int)EXCCAUSE_ALLOCA)
            continue;  /* handled by ESP-IDF / windowed ABI */
        xt_set_exception_handler(i, xtensa_fault_handler);
    }
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
