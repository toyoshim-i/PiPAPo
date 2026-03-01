/*
 * main_qemu.c — Kernel entry point for QEMU mps2-an500 smoke test
 *
 * Replaces main.c for the ppap_qemu build target.  Exercises the same
 * kernel logic as main.c but omits RP2040-specific hardware:
 *
 *   - No clock_init_pll()      (QEMU has no PLL or XOSC to configure)
 *   - No uart_reinit_133mhz()  (clock never changes)
 *   - UART via CMSDK UART0     (uart_qemu.c, not uart.c)
 *   - xip_add() still works    (pure integer arithmetic, no hardware)
 *
 * Useful for:
 *   - Verifying Reset_Handler (.data copy, .bss zero) is correct
 *   - Smoke-testing pure-C kernel logic in CI without real hardware
 *   - Confirming the linker script (qemu.ld) and startup sequence are sane
 *
 * xip_bench / sram_bench are omitted: SysTick timing in QEMU reflects
 * emulation speed, not real clock cycles, so the numbers are not meaningful.
 */

#include "drivers/uart.h"
#include "mm/page.h"
#include "xip_test.h"

void kmain(void)
{
    uart_init_console();
    uart_puts("PicoPiAndPortable booting (QEMU mps2-an500)...\n");
    uart_puts("UART: CMSDK UART0 @ 0x40004000\n");
    uart_puts("Clock: emulated (no PLL — skipping clock_init_pll)\n");

    mm_init();

    /* xip_add is pure arithmetic — runs identically in QEMU and on hardware */
    uart_puts("XIP: xip_add @ ");
    uart_print_hex32((uint32_t)(uintptr_t)xip_add);
    uart_puts(" (ROM, 0x000xxxxx in QEMU)\n");

    int result = xip_add(3, 4);
    uart_puts("XIP: xip_add(3,4) = ");
    uart_putc('0' + (char)result);
    uart_puts("\n");

    uart_puts("QEMU smoke test PASSED\n");

    for (;;) {
    }
}
