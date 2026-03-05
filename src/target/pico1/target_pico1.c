/*
 * target_pico1.c — Target implementation for official Raspberry Pi Pico
 *
 * Pico 1: RP2040, 2 MB flash, no SD card, dual-core.
 * No SPI bus — omits spi_init() and sd_init().
 */

#include "../target.h"
#include "pico1.h"
#include "drivers/uart.h"
#include "drivers/clock.h"
#include "mm/mpu.h"

#ifdef PPAP_TESTS
#include "ktest.h"
#endif

void target_early_init(void)
{
    uart_init_console();
    uart_puts("PicoPiAndPortable booting... [pico1]\n");
    uart_puts("UART: 115200 bps @ 12 MHz XOSC\n");
    uart_flush();
    clock_init_pll();
    uart_reinit_133mhz();
    uart_puts("System clock: 133 MHz\n");
    /* No SPI init — pico1 has no SD card slot */
}

void target_late_init(void)
{
    /* No SD card to initialize */
    uart_flush();
    uart_init_irq();
    uart_puts("UART: switched to interrupt-driven mode\n");
    mpu_init();
    /* core1_launch moved to kmain — must run after init gets PID 1 */
}

void target_post_mount(void)
{
#ifdef PPAP_TESTS
    ktest_run_all();
#endif
}

const char *target_init_path(void)
{
#ifdef PPAP_TESTS
    return "/bin/runtests";
#else
    return "/sbin/init";
#endif
}

uint32_t target_caps(void)
{
    return TARGET_CAP_CORE1 | TARGET_CAP_REALUART;
    /* No TARGET_CAP_SD, no TARGET_CAP_SPI */
}
