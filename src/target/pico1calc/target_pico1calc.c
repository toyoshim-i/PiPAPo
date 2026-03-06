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
#include "drivers/i2c.h"
#include "drivers/spi_lcd.h"
#include "drivers/lcd.h"
#include "drivers/kbd.h"
#include "drivers/fbcon.h"
#include "kernel/fd/tty.h"
#include "kernel/proc/sched.h"
#include "klog.h"
#include "mm/mpu.h"
#include "errno.h"

/* ── LCD + keyboard TTY backend ─────────────────────────────────────────── */

static int fbcon_getc_wrapper(void)  { return kbd_poll(); }
static int fbcon_avail_wrapper(void) { return kbd_poll_avail(); }
static int fbcon_get_cols(void)      { return fbcon_cols(); }
static int fbcon_get_rows(void)      { return fbcon_rows(); }

static const tty_backend_t fbcon_backend = {
    .putc     = fbcon_putc,
    .flush    = fbcon_flush,
    .getc     = fbcon_getc_wrapper,
    .rx_avail = fbcon_avail_wrapper,
    .get_cols = fbcon_get_cols,
    .get_rows = fbcon_get_rows,
};

#ifdef PPAP_TESTS
#include "ktest.h"
#endif

void target_early_init(void)
{
    uart_init_console();
    klog("PicoPiAndPortable booting... [pico1calc]\n");
    klog("UART: 115200 bps @ 12 MHz XOSC\n");
    uart_flush();
    klog("PLL: configuring...\n");
    uart_flush();
    clock_init_pll();
    uart_reinit_133mhz();
    klog("System clock: 133 MHz\n");
    spi_init(400000);
    klog("SPI0: initialised at 400 kHz\n");
    /* Probe I2C first to detect PicoCalc carrier board (STM32 keyboard
     * controller).  LCD init is gated on this because PL022 SPI master
     * mode completes transfers even without a slave, so spi_lcd_ok()
     * cannot detect a missing LCD. */
    i2c_init();
    klog("I2C1: initialised at 10 kHz\n");
    kbd_init();
    if (kbd_present()) {
        spi_lcd_init();
        klog("SPI1: LCD initialised at 33 MHz\n");
        lcd_init();
        klog("LCD: ST7796S initialised (320x320 RGB565)\n");
        fbcon_init();
        klog("FBCON: text console initialised (40x20)\n");
        tty_set_backend(&fbcon_backend);
        sched_set_input_poll(fbcon_avail_wrapper);
        klog("TTY: backend switched to LCD+keyboard\n");
    } else {
        klog("PicoCalc peripherals not detected (skipping LCD/fbcon)\n");
    }
}

void target_late_init(void)
{
    int rc = sd_init();
    if (rc == 0)
        klog("SD: card initialised, mmcblk0 registered\n");
    else if (rc == -ENODEV)
        klog("SD: no card detected (skipping)\n");
    else
        klogf("SD: init failed (err=%u)\n", (uint32_t)(-(int)rc));

    uart_flush();
    uart_init_irq();
    klog("UART: switched to interrupt-driven mode\n");
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
    uint32_t caps = TARGET_CAP_SD | TARGET_CAP_SPI | TARGET_CAP_CORE1
                  | TARGET_CAP_REALUART;
    if (kbd_present())
        caps |= TARGET_CAP_DISPLAY | TARGET_CAP_KBD;
    return caps;
}
