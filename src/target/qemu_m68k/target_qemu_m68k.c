/*
 * target_qemu_m68k.c — Target implementation for QEMU virt m68k
 *
 * Phase C: shared kernel subsystems compiled for m68k.
 * Still uses a target-specific kmain() until all subsystems are ported;
 * Phase C Step 7 will switch to the shared main.c.
 *
 * QEMU virt m68k: RAM at 0x0, Goldfish TTY at 0xFF008000.
 * Run with:
 *   qemu-system-m68k -machine virt -cpu m68000 -nographic \
 *       -kernel ppap_qemu_m68k.elf
 */

#include "drivers/uart.h"
#include "mm/page.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "klog.h"
#include "arch/arch.h"

/* ── Stubs for subsystems not yet ported to m68k ─────────────────────── */

/* tty_rx_notify is called from sched_timer_tick's input polling path.
 * No tty subsystem on m68k yet — stub it out. */
void tty_rx_notify(int idx) { (void)idx; }

/* ── Phase C kernel entry ─────────────────────────────────────────────── *
 *
 * Exercises the shared subsystems (mm, proc, sched) that now compile
 * for m68k.  Phase C Step 7 will switch to the shared main.c.
 * ────────────────────────────────────────────────────────────────────────── */

void kmain(void)
{
    uart_init_console();

    klog("PicoPiAndPortable booting... [qemu_m68k]\n");
    klog("CPU: Motorola 68000\n");

    /* Memory manager */
    mm_init();

    /* Process table */
    proc_init();

    /* ── Context switch smoke test ──────────────────────────────────────── */
    /* Allocate a second process and do cooperative context switching
     * via TRAP #1 (sched_yield).  If switch.S works correctly, the two
     * processes alternate printing their PIDs. */

    /* Give thread 0 a stack page so the context switch can save its state */
    proc_table[0].stack_page = page_alloc();
    if (!proc_table[0].stack_page) {
        klog("PANIC: no page for thread 0 stack\n");
        for (;;) __asm__ volatile ("nop");
    }

    /* Allocate process 1 */
    pcb_t *p1 = proc_alloc();
    if (!p1) {
        klog("PANIC: proc_alloc failed\n");
        for (;;) __asm__ volatile ("nop");
    }
    p1->stack_page = page_alloc();
    if (!p1->stack_page) {
        klog("PANIC: no page for p1 stack\n");
        for (;;) __asm__ volatile ("nop");
    }

    /* Set up process 1's stack to enter test_process1 */
    extern void test_process1(void);
    proc_setup_stack(p1, test_process1, 0);
    p1->state = PROC_RUNNABLE;
    klogf("SCHED: p1 pid=%u allocated, stack @ %x\n",
          p1->pid, (uint32_t)(uintptr_t)p1->stack_page);

    klog("SCHED: starting cooperative context switch test\n");

    /* Thread 0 yields back and forth with process 1 */
    for (int i = 0; i < 4; i++) {
        klogf("Thread 0: iteration %u, yielding...\n", (uint32_t)i);
        sched_yield();
    }

    klog("Phase C context switch test PASSED\n");

    for (;;)
        __asm__ volatile ("nop");
}

/* Test process 1 — runs on its own stack, yields back to thread 0 */
void test_process1(void)
{
    for (int i = 0; i < 4; i++) {
        klogf("Process 1: iteration %u, yielding...\n", (uint32_t)i);
        sched_yield();
    }

    /* Mark ourselves done and yield forever */
    current->state = PROC_FREE;
    for (;;)
        sched_yield();
}
