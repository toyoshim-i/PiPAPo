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
#include "drivers/uart.h"
#include <stddef.h>   /* NULL */

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
