/*
 * target_qemu_m68k.c — Target implementation for QEMU virt m68k
 *
 * Phase C: shared kernel subsystems compiled for m68k.
 * Exercises the memory manager, process table, context switch
 * (both cooperative and preemptive via Goldfish RTC timer).
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

/* ── Timer driver ────────────────────────────────────────────────────── */

/* Defined in drivers/timer_qemu_m68k.c */
extern void timer_init(void);

/* ── Test processes ──────────────────────────────────────────────────── */

/* Volatile counter to prevent the compiler from optimising the spin loop
 * into an infinite loop or removing it entirely. */
static volatile uint32_t counter1 = 0;
static volatile uint32_t counter2 = 0;

/* Process 1: spins and counts.  Preemptive scheduling switches it out. */
void test_process1(void)
{
    for (;;) {
        counter1++;
        if ((counter1 % 100000u) == 0u)
            klogf("P1: count=%u\n", counter1);
    }
}

/* Process 2: spins and counts.  Preemptive scheduling switches it out. */
void test_process2(void)
{
    for (;;) {
        counter2++;
        if ((counter2 % 100000u) == 0u)
            klogf("P2: count=%u\n", counter2);
    }
}

/* ── Phase C kernel entry ─────────────────────────────────────────────── *
 *
 * Exercises the shared subsystems (mm, proc, sched) that now compile
 * for m68k.  Tests both cooperative yield and preemptive timer-driven
 * context switching.
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

    /* ── Cooperative context switch test ──────────────────────────────── */
    /* Quick sanity check: allocate a process, yield back and forth. */
    proc_table[0].stack_page = page_alloc();
    if (!proc_table[0].stack_page) {
        klog("PANIC: no page for thread 0 stack\n");
        for (;;) __asm__ volatile ("nop");
    }

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

    /* Set up a simple yield-test process */
    extern void yield_test_process(void);
    proc_setup_stack(p1, yield_test_process, 0);
    p1->state = PROC_RUNNABLE;

    klog("SCHED: cooperative yield test...\n");
    for (int i = 0; i < 4; i++) {
        klogf("Thread 0: iteration %u, yielding...\n", (uint32_t)i);
        sched_yield();
    }
    klog("SCHED: cooperative yield test PASSED\n");

    /* Clean up the yield-test process */
    p1->state = PROC_FREE;

    /* ── Preemptive scheduling test ───────────────────────────────────── */
    klog("SCHED: starting preemptive scheduling test...\n");

    /* Create two busy processes */
    pcb_t *pa = proc_alloc();
    pa->stack_page = page_alloc();
    proc_setup_stack(pa, test_process1, 0);
    pa->state = PROC_RUNNABLE;

    pcb_t *pb = proc_alloc();
    pb->stack_page = page_alloc();
    proc_setup_stack(pb, test_process2, 0);
    pb->state = PROC_RUNNABLE;

    /* Initialize the Goldfish RTC timer (10 ms periodic) */
    timer_init();

    /* Start the scheduler (enables interrupts) */
    sched_start();

    /* Thread 0 (idle): spin and report tick count periodically */
    uint32_t last_report = 0;
    for (;;) {
        uint32_t ticks = sched_get_ticks();
        if (ticks >= last_report + 100u) {
            last_report = ticks;
            klogf("IDLE: ticks=%u  P1=%u  P2=%u\n",
                  ticks, counter1, counter2);

            /* After 500 ticks (5 seconds), declare success */
            if (ticks >= 500u) {
                klog("Phase C preemptive scheduling test PASSED\n");
                for (;;) __asm__ volatile ("nop");
            }
        }
    }
}

/* Yield-test process — runs on its own stack, yields back to thread 0 */
void yield_test_process(void)
{
    for (int i = 0; i < 4; i++) {
        klogf("Yield P1: iteration %u, yielding...\n", (uint32_t)i);
        sched_yield();
    }

    /* Mark ourselves done and yield forever */
    current->state = PROC_FREE;
    for (;;)
        sched_yield();
}
