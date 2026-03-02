/*
 * proc.c — Process table and PCB lifecycle
 *
 * Manages a flat array of PROC_MAX process control blocks.  proc_table[0]
 * is permanently reserved for the initial kernel/init thread; slots 1..7
 * are available for user processes allocated via proc_alloc().
 *
 * proc_alloc() scans for a PROC_FREE slot — O(PROC_MAX) = O(8), negligible.
 * PIDs are assigned from a monotonically increasing counter so they are
 * unique for the lifetime of the kernel (no wraparound in Phase 1).
 */

#include "proc.h"
#include "../mm/page.h"   /* PAGE_SIZE — for proc_setup_stack */
#include "drivers/uart.h" /* uart_puts, uart_print_dec — for proc_init diagnostics */
#include <stddef.h>   /* NULL, offsetof */

/* Verify PCB_SP_OFFSET matches the actual struct layout at compile time.
 * If this fires, update PCB_SP_OFFSET in proc.h to match offsetof(pcb_t,sp). */
_Static_assert(offsetof(pcb_t, sp) == PCB_SP_OFFSET,
               "PCB_SP_OFFSET does not match offsetof(pcb_t, sp) — update proc.h");

/* ── Globals ─────────────────────────────────────────────────────────────── */

pcb_t  proc_table[PROC_MAX];
pcb_t *current = NULL;

/* Monotonically increasing PID counter.  Starts at 1; pid 0 is the kernel. */
static pid_t next_pid = 1;

/* ── Public API ──────────────────────────────────────────────────────────── */

void proc_init(void)
{
    /* Zero all slots and mark them free */
    for (uint32_t i = 0u; i < PROC_MAX; i++) {
        __builtin_memset(&proc_table[i], 0, sizeof(pcb_t));
        proc_table[i].state = PROC_FREE;
    }

    /* Pre-initialise slot 0 as the initial kernel thread.
     * stack_page is NULL: this thread runs on the initial kernel stack
     * set up by startup.S; no page_alloc() needed. */
    proc_table[0].pid             = 0;
    proc_table[0].ppid            = 0;
    proc_table[0].state           = PROC_RUNNABLE;
    proc_table[0].ticks_remaining = PROC_DEFAULT_TICKS;

    current = &proc_table[0];

    /* ── Print boot diagnostic ─────────────────────────────────────────── */
    uart_puts("PROC: process table  slots=");
    uart_print_dec(PROC_MAX);
    uart_puts("  (pid 0 = kernel, pids 1–");
    uart_print_dec((uint32_t)(PROC_MAX - 1u));
    uart_puts(" available)\n");

    /* ── Self-test ─────────────────────────────────────────────────────── */
    /* Allocate a slot, verify it looks sane, then free it. */
    pcb_t *p = proc_alloc();

    uint32_t ok = (p != NULL)
               && (p->pid   == 1)
               && (p->state == PROC_FREE)   /* still FREE — caller sets state */
               && (current  == &proc_table[0])
               && (current->state == PROC_RUNNABLE);

    if (p)
        proc_free(p);

    uart_puts("PROC: self-test ");
    uart_puts(ok ? "PASSED\n" : "FAILED\n");
}

pcb_t *proc_alloc(void)
{
    /* Scan slots 1..PROC_MAX-1; slot 0 belongs to the kernel thread */
    for (uint32_t i = 1u; i < PROC_MAX; i++) {
        if (proc_table[i].state == PROC_FREE) {
            __builtin_memset(&proc_table[i], 0, sizeof(pcb_t));
            proc_table[i].pid = next_pid++;
            /* Init process group / session to self */
            proc_table[i].pgid = proc_table[i].pid;
            proc_table[i].sid  = proc_table[i].pid;
            proc_table[i].umask_val = 022;
            /* state left as PROC_FREE — caller sets it to PROC_RUNNABLE
             * only after filling in stack_page and setting up the stack frame */
            return &proc_table[i];
        }
    }
    return NULL;   /* all slots occupied */
}

void proc_free(pcb_t *p)
{
    if (!p)
        return;
    p->state = PROC_FREE;
}

void proc_setup_stack(pcb_t *p, void (*entry)(void))
{
    /*
     * Build the initial stack frame that PendSV_Handler will restore on
     * the first context switch into this process.
     *
     * Stack grows downward from the top of the 4 KB page.  We push two
     * layers (high address to low):
     *
     *  1. Hardware exception frame (8 words): the CPU pops this when
     *     PendSV does `bx lr` with EXC_RETURN = 0xFFFFFFFD.
     *
     *  2. Software callee-saved frame (8 words): PendSV_Handler loads
     *     r4–r11 from here, then sets PSP to the start of layer 1.
     *
     * p->sp is set to the bottom of layer 2 (= the `r4` slot).
     */
    uint32_t *sp = (uint32_t *)((uint8_t *)p->stack_page + PAGE_SIZE);

    /* ── Layer 1: hardware exception frame (high → low) ──────────────── */
    *--sp = 0x01000000u;                  /* xpsr: Thumb bit (T=1)       */
    *--sp = (uint32_t)entry & ~1u;        /* pc: entry point (bit0 clear)*/
    *--sp = 0xFFFFFFFDu;                  /* lr: EXC_RETURN thread/PSP   */
    *--sp = 0u;                           /* r12                         */
    *--sp = 0u;                           /* r3                          */
    *--sp = 0u;                           /* r2                          */
    *--sp = 0u;                           /* r1                          */
    *--sp = 0u;                           /* r0                          */

    /* ── Layer 2: software callee-saved frame (r11..r4, high → low) ─── */
    *--sp = 0u;   /* r11 */
    *--sp = 0u;   /* r10 */
    *--sp = 0u;   /* r9  */
    *--sp = 0u;   /* r8  */
    *--sp = 0u;   /* r7  */
    *--sp = 0u;   /* r6  */
    *--sp = 0u;   /* r5  */
    *--sp = 0u;   /* r4  */   /* ← pcb_t.sp points here */

    p->sp = (uint32_t)(uintptr_t)sp;
    p->ticks_remaining = PROC_DEFAULT_TICKS;
}
