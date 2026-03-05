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
#include "sched.h"        /* sched_get_ticks — for start_time */
#include "../mm/page.h"   /* PAGE_SIZE — for proc_setup_stack */
#include "../spinlock.h"  /* SPIN_PROC */
#include "drivers/uart.h" /* uart_puts, uart_print_dec — for proc_init diagnostics */
#include "hw/cortex_m0plus.h" /* XPSR_THUMB_BIT, EXC_RETURN_THREAD_PSP */
#include <stddef.h>   /* NULL, offsetof */

/* Default file creation mask (octal 022 → owner rw, group/other r) */
#define DEFAULT_UMASK  022

/* Verify PCB_SP_OFFSET matches the actual struct layout at compile time.
 * If this fires, update PCB_SP_OFFSET in proc.h to match offsetof(pcb_t,sp). */
_Static_assert(offsetof(pcb_t, sp) == PCB_SP_OFFSET,
               "PCB_SP_OFFSET does not match offsetof(pcb_t, sp) — update proc.h");

/* ── Globals ─────────────────────────────────────────────────────────────── */

pcb_t  proc_table[PROC_MAX];
pcb_t *current_core[2] = { NULL, NULL };

/* Indirect core-ID register pointer for assembly (switch.S, svc.S).
 * Points to SIO_CPUID on RP2040, or core_id_zero on QEMU.
 * Assembly does: ldr rN, =core_id_reg; ldr rN, [rN]; ldr rN, [rN]
 * → two loads, no branches, works on both platforms. */
static uint32_t core_id_zero = 0;
volatile uint32_t *core_id_reg = &core_id_zero;

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
    /* comm is set to "init" once do_execve() runs; "kernel" for now */
    __builtin_memcpy(proc_table[0].comm, "kernel", 7);

    current_core[0] = &proc_table[0];

    /* Point assembly's core_id_reg at the SIO_CPUID register on RP2040.
     * On QEMU (no SIO), it stays pointing at core_id_zero → always 0. */
    if (spin_have_hw())
        core_id_reg = (volatile uint32_t *)0xD0000000u;

    /* ── Print boot diagnostic ─────────────────────────────────────────── */
    uart_puts("PROC: process table  slots=");
    uart_print_dec(PROC_MAX);
    uart_puts("  (pid 0 = kernel, pids 1–");
    uart_print_dec((uint32_t)(PROC_MAX - 1u));
    uart_puts(" available)\n");

#ifdef PPAP_TESTS
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

    /* Reset PID counter so the first real process gets PID 1.
     * busybox init requires PID == 1. */
    next_pid = 1;

    uart_puts("PROC: self-test ");
    uart_puts(ok ? "PASSED\n" : "FAILED\n");
#endif
}

pcb_t *proc_alloc(void)
{
    uint32_t saved = spin_lock_irqsave(SPIN_PROC);
    pcb_t *result = NULL;

    /* Scan slots 1..PROC_MAX-1; slot 0 belongs to the kernel thread */
    for (uint32_t i = 1u; i < PROC_MAX; i++) {
        if (proc_table[i].state == PROC_FREE) {
            __builtin_memset(&proc_table[i], 0, sizeof(pcb_t));
            proc_table[i].pid = next_pid++;
            /* pgid and sid are left at 0 (from memset).
             * sys_vfork copies them from the parent (like real fork).
             * Only setsid/setpgid should change them explicitly. */
            proc_table[i].umask_val = DEFAULT_UMASK;
            proc_table[i].running_on_core = -1;
            proc_table[i].start_time = sched_get_ticks();
            /* state left as PROC_FREE — caller sets it to PROC_RUNNABLE
             * only after filling in stack_page and setting up the stack frame */
            result = &proc_table[i];
            break;
        }
    }

    spin_unlock_irqrestore(SPIN_PROC, saved);
    return result;
}

void proc_free(pcb_t *p)
{
    if (!p)
        return;
    uint32_t saved = spin_lock_irqsave(SPIN_PROC);
    p->state = PROC_FREE;
    spin_unlock_irqrestore(SPIN_PROC, saved);
}

void proc_setup_stack(pcb_t *p, void (*entry)(void), uint32_t user_sp)
{
    /*
     * Build the initial stack frame that PendSV_Handler will restore on
     * the first context switch into this process.
     *
     * Stack grows downward.  We push two layers (high address to low):
     *
     *  1. Hardware exception frame (8 words): the CPU pops this when
     *     PendSV does `bx lr` with EXC_RETURN = 0xFFFFFFFD.
     *
     *  2. Software callee-saved frame (8 words): PendSV_Handler loads
     *     r4–r11 from here, then sets PSP to the start of layer 1.
     *
     * user_sp is the PSP after the hardware frame pop.  For exec'd
     * processes it points to the argc slot; for plain threads it's
     * the top of the stack page.
     *
     * p->sp is set to the bottom of layer 2 (= the `r4` slot).
     */
    uint32_t *sp;
    if (user_sp)
        sp = (uint32_t *)(uintptr_t)user_sp;
    else
        sp = (uint32_t *)((uint8_t *)p->stack_page + PAGE_SIZE);

    /* ── Layer 1: hardware exception frame (high → low) ──────────────── */
    *--sp = XPSR_THUMB_BIT;              /* xpsr: Thumb bit (T=1)       */
    *--sp = (uint32_t)entry & ~1u;        /* pc: entry point (bit0 clear)*/
    *--sp = EXC_RETURN_THREAD_PSP;        /* lr: EXC_RETURN thread/PSP   */
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
