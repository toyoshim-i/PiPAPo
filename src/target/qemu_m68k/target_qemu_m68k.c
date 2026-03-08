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

    klog("Phase C bringup: shared subsystems OK\n");

    /* Halt — timer ISR and context switch not yet implemented.
     * Use a simple busy loop; STOP triggers a QEMU assertion. */
    for (;;)
        __asm__ volatile ("nop");
}
