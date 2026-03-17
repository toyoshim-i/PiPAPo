/*
 * target_pico1calc.c — Target implementation for ClockworkPi PicoCalc
 *
 * PicoCalc: RP2040, 16 MB flash, SPI0 SD card, dual-core.
 * Full hardware feature set: PLL, SPI, SD, IRQ UART, MPU, Core 1.
 */

#include "../target.h"
#include "pico1calc.h"
#include "drivers/uart.h"
#include "drivers/arch/arm_m/uart_rp2040.h"
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
#include "kernel/fs/devfs.h"
#include "kernel/fs/procfs.h"
#include "klog.h"
#include "mm/mpu.h"
#include "errno.h"

/* ── LCD + keyboard TTY backend ─────────────────────────────────────────── */

/* Small ring buffer for keyboard characters.
 *
 * kbd_poll() (I2C + escape sequence state) is only ever called from the
 * idle loop on Core 0 (via fbcon_avail_wrapper).  Process-context reads
 * (fbcon_getc_wrapper) consume from this ring buffer only — they MUST NOT
 * call kbd_poll() directly because kbd.c's internal state (seq_buf, seq_pos)
 * is not thread-safe and the two callers can run on different cores. */
#define KBD_RING_SIZE 16u   /* power of 2 for mask wrap */
static volatile char  kbd_ring[KBD_RING_SIZE];
static volatile uint8_t kbd_ring_head;  /* written by idle loop (Core 0) */
static volatile uint8_t kbd_ring_tail;  /* read by process (any core)    */

static inline int kbd_ring_empty(void)
{
    return kbd_ring_head == kbd_ring_tail;
}

static inline void kbd_ring_put(char c)
{
    uint8_t next = (kbd_ring_head + 1u) & (KBD_RING_SIZE - 1u);
    if (next == kbd_ring_tail)
        return;   /* full — drop character */
    kbd_ring[kbd_ring_head] = c;
    kbd_ring_head = next;
}

static inline int kbd_ring_get(void)
{
    if (kbd_ring_empty())
        return -1;
    int c = (unsigned char)kbd_ring[kbd_ring_tail];
    kbd_ring_tail = (kbd_ring_tail + 1u) & (KBD_RING_SIZE - 1u);
    return c;
}

static int fbcon_getc_wrapper(void)
{
    return kbd_ring_get();
}

static int fbcon_avail_wrapper(void)
{
    if (!kbd_ring_empty())
        return 1;

    /* Drain up to 8 key events per poll cycle.  The loop MUST be bounded:
     * this runs in the idle loop on Core 0.  An unbounded loop would hang
     * if kbd_poll() keeps returning -1 due to an I2C FIFO read error while
     * kbd_poll_avail() still reports data available. */
    for (int tries = 0; tries < 8 && kbd_poll_avail(); tries++) {
        int ch = kbd_poll();
        if (ch < 0)
            break;   /* I2C error — stop polling this cycle */
        /* Deliver Ctrl-C immediately on tty1 so compute-bound foreground
         * tasks do not need to be blocked in read(). */
        if (ch == 0x03 && tty_signal_intr(TTY_DISPLAY))
            continue;
        kbd_ring_put((char)ch);
    }
    return !kbd_ring_empty();
}

static int fbcon_get_cols(void)      { return fbcon_cols(); }
static int fbcon_get_rows(void)      { return fbcon_rows(); }

/* ── LCD backlight (STM32 I2C register 0x05) ──────────────────────────── */

#define PICO_STM32_ADDR  0x1F
#define REG_ID_BKL       0x05
#define REG_ID_BAT       0x0B
#define REG_ID_OFF       0x0E
#define REG_WRITE_MASK   0x80   /* STM32 FW uses bit 7 to flag register writes */

static int bl_i2c_get(uint8_t *val)
{
    return i2c_read_reg(PICO_STM32_ADDR, REG_ID_BKL, val, 1);
}

static int bl_i2c_set(uint8_t val)
{
    return i2c_write_reg(PICO_STM32_ADDR, REG_ID_BKL | REG_WRITE_MASK, &val, 1);
}

/* ── Battery and power (STM32 I2C registers 0x0B, 0x0E) ──────────────── */

static int bat_i2c_read(uint8_t *buf, int len)
{
    /* STM32 returns 2 bytes: [reg_echo, percentage].
     * Percentage bits 0-6 = 0-100%, bit 7 = charging flag.
     * Extract into buf[0] = percentage byte. */
    uint8_t raw[2];
    int rc = i2c_read_reg(PICO_STM32_ADDR, REG_ID_BAT, raw, 2);
    if (rc < 0)
        return rc;
    buf[0] = raw[1];   /* percentage (bit 7 = charging) */
    if (len > 1)
        buf[1] = 0;
    return 0;
}

static int power_i2c_off(void)
{
    uint8_t val = 1;
    return i2c_write_reg(PICO_STM32_ADDR, REG_ID_OFF | REG_WRITE_MASK, &val, 1);
}

/* ── TTY backend ──────────────────────────────────────────────────────── */

static const tty_backend_t fbcon_backend = {
    .putc     = fbcon_putc,
    .flush    = fbcon_flush_deferred,
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
    uart_init();
    klog("PiPAPo booting... [pico1calc]\n");
    klog("UART: 115200 bps @ 12 MHz XOSC\n");
    klog("PLL: configuring...\n");
    uart_tx_drain();           /* drain at 12 MHz; also disables UART0 NVIC */
    clock_init_pll();          /* switch clk_sys to 133 MHz                 */
    uart_reinit_133mhz();     /* set 133 MHz divisors                      */
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
        if (!spi_lcd_ok())
            klog("LCD: *** SPI timeout during init ***\n");
        else
            klog("LCD: ST7365P initialised (320x320 RGB565)\n");
        fbcon_init();
        klog("FBCON: text console initialised (40x20)\n");
        klog_set_mirror(fbcon_putc, fbcon_flush_deferred);
        klog("KLOG: output mirrored to LCD\n");
        tty_set_backend(TTY_DISPLAY, &fbcon_backend);
        sched_set_input_poll(fbcon_avail_wrapper, TTY_DISPLAY);
        sched_set_display_poll(fbcon_poll_flush);
        klog("TTY: backend switched to LCD+keyboard\n");
        devfs_set_backlight(bl_i2c_get, bl_i2c_set);
        bl_i2c_set(128);
        klog("BACKLIGHT: set to 128/255\n");
        procfs_set_battery(bat_i2c_read);
        devfs_set_power(power_i2c_off);
        klog("POWER: battery monitor and power-off registered\n");
    } else {
        klog("PicoCalc peripherals not detected (skipping LCD/fbcon)\n");
    }
}

void target_late_init(void)
{
    /* TODO: SD init disabled — spi_xfer() hangs on the first SPI0 data
     * transfer after UF2 bootloader warm boot.  The PL022 accepts TX data
     * but never produces RX data, suggesting clk_peri or GPIO mux issue.
     * Investigate: try SPI0 loopback test, verify clk_peri, check if the
     * bootloader's boot2 reconfigures pin mux for QSPI that conflicts. */
#if 0
    int rc = sd_init();
    if (rc == 0)
        klog("SD: card initialised, mmcblk0 registered\n");
    else if (rc == -ENODEV)
        klog("SD: no card detected (skipping)\n");
    else
        klogf("SD: init failed (err=%u)\n", (uint32_t)(-(int)rc));
#else
    klog("SD: disabled (SPI0 hang under investigation)\n");
#endif

    while (uart_getc() >= 0) ;   /* drain boot noise from RX ring */
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
#ifdef PPAP_TESTS_EXTENDED
    return "/bin/runtests_ext";
#else
    return "/bin/runtests";
#endif
#else
    return "/sbin/init";
#endif
}

const char *target_name(void)
{
    return "pico1calc";
}

uint32_t target_caps(void)
{
    uint32_t caps = TARGET_CAP_SD | TARGET_CAP_SPI | TARGET_CAP_CORE1
                  | TARGET_CAP_REALUART;
    if (kbd_present())
        caps |= TARGET_CAP_DISPLAY | TARGET_CAP_KBD;
    return caps;
}

/* ── ARM FPB hardware breakpoints (native ptrace backend) ───────────────── */

#define ARM_DEMCR_ADDR  (0xE000EDFCu)
#define ARM_FPB_BASE    (0xE0002000u)
#define ARM_FPB_CTRL    (*(volatile uint32_t *)(ARM_FPB_BASE + 0x00u))
#define ARM_FPB_COMP(i) (*(volatile uint32_t *)(ARM_FPB_BASE + 0x08u + ((i) * 4u)))
#define ARM_DEMCR       (*(volatile uint32_t *)ARM_DEMCR_ADDR)

#define ARM_DEMCR_TRCENA     (1u << 24)
#define ARM_FPB_CTRL_ENABLE  (1u << 0)

static uint32_t arm_fpb_code_slots(void)
{
    uint32_t ctrl = ARM_FPB_CTRL;
    uint32_t n = ((ctrl >> 4) & 0xFu) | (((ctrl >> 12) & 0x7u) << 4);

    if (n > 4u)
        n = 4u;
    return n;
}

uint32_t target_debug_hwbp_slots(void)
{
    return arm_fpb_code_slots();
}

int target_debug_hwbp_set(uint32_t slot, uint32_t addr)
{
    uint32_t slots = arm_fpb_code_slots();
    uint32_t replace;
    uint32_t comp;

    if (slot >= slots)
        return -1;

    ARM_DEMCR |= ARM_DEMCR_TRCENA;
    ARM_FPB_CTRL |= ARM_FPB_CTRL_ENABLE;

    replace = (addr & 0x2u) ? 2u : 1u;
    comp = (addr & 0x1FFFFFFCu) | (replace << 30) | 1u;
    ARM_FPB_COMP(slot) = comp;
    return 0;
}

int target_debug_hwbp_clear(uint32_t slot)
{
    uint32_t slots = arm_fpb_code_slots();

    if (slot >= slots)
        return -1;

    ARM_FPB_COMP(slot) = 0;
    return 0;
}
