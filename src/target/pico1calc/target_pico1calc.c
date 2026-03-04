/*
 * target_pico1calc.c — Target implementation for ClockworkPi PicoCalc
 *
 * PicoCalc: RP2040, 16 MB flash, SPI0 SD card, dual-core.
 * Full hardware feature set: PLL, SPI, SD, IRQ UART, MPU, Core 1.
 */

#include "../target.h"
#include "pico1calc.h"
#include "drivers/uart.h"
#include "drivers/clock.h"
#include "drivers/spi.h"
#include "drivers/sd.h"
#include "mm/mpu.h"
#include "smp.h"
#include "errno.h"

#ifdef PPAP_TESTS
#include "test/ktest.h"
#endif

void target_early_init(void)
{
    uart_init_console();
    uart_puts("PicoPiAndPortable booting... [pico1calc]\n");
    uart_puts("UART: 115200 bps @ 12 MHz XOSC\n");
    uart_flush();
    clock_init_pll();
    uart_reinit_133mhz();
    uart_puts("System clock: 133 MHz\n");
    spi_init(400000);
    uart_puts("SPI0: initialised at 400 kHz\n");
}

void target_late_init(void)
{
    int rc = sd_init();
    if (rc == 0)
        uart_puts("SD: card initialised, mmcblk0 registered\n");
    else if (rc == -ENODEV)
        uart_puts("SD: no card detected (skipping)\n");
    else {
        uart_puts("SD: init failed (err=");
        uart_print_dec((uint32_t)(-(int)rc));
        uart_puts(")\n");
    }

    uart_flush();
    uart_init_irq();
    uart_puts("UART: switched to interrupt-driven mode\n");
    mpu_init();
    core1_launch(core1_io_worker);
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
    return TARGET_CAP_SD | TARGET_CAP_SPI | TARGET_CAP_CORE1 | TARGET_CAP_REALUART;
}
