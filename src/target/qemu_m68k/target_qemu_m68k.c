/*
 * target_qemu_m68k.c — Target implementation for QEMU virt m68k
 *
 * Phase B: minimal bringup — boot to UART output, no kernel services yet.
 *
 * QEMU virt m68k: RAM at 0x0, Goldfish TTY at 0xFF002000.
 * Run with:
 *   qemu-system-m68k -machine virt -cpu m68000 -nographic \
 *       -kernel ppap_qemu_m68k.elf
 */

#include "drivers/uart.h"

/* m68k_switch_pending: context switch flag used by arch_yield().
 * Defined here since we don't have a scheduler yet in Phase B. */
volatile unsigned int m68k_switch_pending = 0;

/* ── Phase B minimal kernel entry ─────────────────────────────────────── *
 *
 * In Phase B we provide our own kmain() instead of using the shared
 * main.c, which depends on subsystems not yet ported (proc, vfs, etc.).
 * Phase C will switch to the shared kmain() after porting those.
 * ────────────────────────────────────────────────────────────────────────── */

void kmain(void)
{
    uart_init_console();

    uart_puts("PicoPiAndPortable booting... [qemu_m68k]\n");
    uart_puts("UART: Goldfish TTY @ 0xFF008000\n");
    uart_puts("CPU: Motorola 68000\n");
    uart_puts("Clock: emulated (no PLL)\n");
    uart_puts("\n");
    uart_puts("Phase B bringup complete.\n");

    /* Halt — no scheduler yet.
     * Use a simple busy loop; `stop` triggers a QEMU assertion bug. */
    for (;;)
        __asm__ volatile ("nop");
}
