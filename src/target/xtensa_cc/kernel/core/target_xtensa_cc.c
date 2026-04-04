/*
 * target_xtensa_cc.c — Target implementation for M5Stack CardComputer
 *
 * ESP32-S3 (STAMP S3 module), 8 MB flash, 512 KB SRAM.
 * ST7789V2 display, 56-key keyboard, microSD, speaker, IR.
 *
 * CC-1: Minimal serial-only boot.  Display, keyboard, and SD are
 * deferred to CC-4, CC-5, and CC-6 respectively.
 *
 * ESP-IDF handles boot, clock, flash cache, and UART initialization.
 * PPAP takes over from app_main() → kmain().
 */

#include <stddef.h>
#include <stdint.h>

#include "target/target.h"
#include "xtensa_cc.h"
#include "kernel/core/klog.h"
#include "sdkconfig.h"
#include "kernel/core/cpu.h"
#include "kernel/core/driver/uart.h"
#include "kernel/core/proc/sched.h"
#include "kernel/vfs/tty.h"

#if !defined(CONFIG_FREERTOS_UNICORE) || !CONFIG_FREERTOS_UNICORE
#error "xtensa_cc requires CONFIG_FREERTOS_UNICORE=y"
#endif

/* ESP-IDF: IRAM_ATTR for ISR placed in IRAM */
#include "esp_attr.h"

/* ESP32-S3 SYSTIMER registers — used to disable FreeRTOS tick alarms
 * before PPAP takes over.  Base: 0x6001F000 (TRM §11.5). */
#define SYSTIMER_BASE           0x6001F000u
#define SYSTIMER_CONF_REG       (*(volatile uint32_t *)(SYSTIMER_BASE + 0x000u))
#define SYSTIMER_TARGET0_CONF   (*(volatile uint32_t *)(SYSTIMER_BASE + 0x030u))
#define SYSTIMER_TARGET1_CONF   (*(volatile uint32_t *)(SYSTIMER_BASE + 0x034u))
#define SYSTIMER_TARGET2_CONF   (*(volatile uint32_t *)(SYSTIMER_BASE + 0x038u))
#define SYSTIMER_INT_ENA_REG    (*(volatile uint32_t *)(SYSTIMER_BASE + 0x068u))
#define SYSTIMER_INT_CLR_REG    (*(volatile uint32_t *)(SYSTIMER_BASE + 0x06Cu))

#ifdef PPAP_TESTS
#include "ktest.h"
#endif

/* ── app_main — ESP-IDF entry point ─────────────────────────────────────── *
 *
 * ESP-IDF calls app_main() after completing its own initialization:
 *   ROM bootloader → second-stage bootloader → FreeRTOS init → app_main()
 *
 * We immediately call kmain() to hand control to the PPAP kernel.
 * ESP-IDF's FreeRTOS scheduler is NOT used — PPAP has its own scheduler.
 */
extern void kmain(void);

void app_main(void)
{
    kmain();
    /* kmain() never returns — idle loop is at the end of kmain(). */
}

/* ── Target interface implementation ────────────────────────────────────── */

/* No-op ISR for stray FreeRTOS interrupts (SYSTIMER on CPU interrupt 12).
 * Must be in IRAM since it runs from interrupt context. */
static void IRAM_ATTR systimer_noop_isr(void *arg) { (void)arg; }

void target_early_init(void)
{
    klog_set_logger(KLOG_LOGGER_PRIMARY, uart_putc, NULL);
    /* FIRST: patch the Xtensa interrupt dispatch table directly to replace
     * the default "xt_unhandled_interrupt" handler for CPU interrupt 12
     * (FreeRTOS's SYSTIMER tick) with our no-op.
     *
     * _xt_interrupt_table is an array of {handler, arg} pairs (8 bytes each).
     * The level-1 ISR dispatcher reads this table and calls the handler.
     * By writing directly, we bypass any API issues. */
    extern void *_xt_interrupt_table[];
    _xt_interrupt_table[12 * 2 + 0] = (void *)systimer_noop_isr;
    _xt_interrupt_table[12 * 2 + 1] = (void *)0;

    /* ESP-IDF has already initialized:
     *   - System clock (240 MHz PLL)
     *   - UART0 console (115200 8N1 via USB CDC or UART pins)
     *   - Flash cache (XIP)
     *   - GPIO subsystem
     *
     * Aggressively shut down FreeRTOS's SYSTIMER tick.
     * FreeRTOS configures SYSTIMER target comparators and maps them to
     * CPU interrupt 12 via the interrupt matrix.  The interrupt is
     * level-triggered — it keeps firing until the hardware source is
     * fully stopped.  FreeRTOS also saves/restores INTENABLE during
     * context switches, so simply writing INTENABLE=0 is insufficient. */
    SYSTIMER_TARGET0_CONF &= ~(1u << 30);  /* stop TARGET0 period mode  */
    SYSTIMER_TARGET1_CONF &= ~(1u << 30);  /* stop TARGET1 period mode  */
    SYSTIMER_TARGET2_CONF &= ~(1u << 30);  /* stop TARGET2 period mode  */
    SYSTIMER_CONF_REG &= ~((1u << 29) | (1u << 30)); /* stop counters   */
    SYSTIMER_INT_ENA_REG = 0;              /* disable alarm interrupts   */
    SYSTIMER_INT_CLR_REG = 0x7u;           /* clear pending alarm flags  */

    /* Disable all CPU interrupts and clear pending bits */
    __asm__ volatile("wsr %0, intclear; rsync" :: "r"(0xFFFFFFFFu));
    __asm__ volatile("wsr %0, intenable; rsync" :: "r"(0));

    klog("PiPAPo booting... [xtensa_cc]\n");
    klogf("System clock: %lu MHz\n", (unsigned long)(PPAP_SYS_HZ / 1000000u));
}

void target_late_init(void)
{
    /* CC-2: start CCOMPARE0 timer for scheduler tick. */
    extern void xtensa_timer_init(void);
    xtensa_timer_init();

    /* CC-3: install syscall + exception handlers. */
    extern void xtensa_trap_init(void);
    xtensa_trap_init();

    /* Register UART RX polling so the idle loop feeds input to TTY.
     * uart_rx_avail() checks UART0 RX FIFO via bare-metal register access
     * (no ROM calls — ROM functions may use `syscall 0` internally, which
     * crashes in kernel mode where KernelExceptionVector doesn't handle it). */
    sched_set_input_poll(uart_rx_avail, TTY_SERIAL);

    /* XT-3.4 bootstrap boundary checks:
     * - FreeRTOS scheduler handoff must stay disabled
     * - PPAP must own timer and trap paths
     * - active interrupt mask must remain PPAP-owned */
    {
        extern uint32_t port_xSchedulerRunning[];
        extern volatile uint32_t xtensa_timer_ready;
        extern volatile uint32_t xtensa_trap_ready;

        if (port_xSchedulerRunning[0] != 0u) {
            klog("XT-3.4: forcing FreeRTOS scheduler handoff off\n");
            port_xSchedulerRunning[0] = 0u;
        }

        if (!xtensa_timer_ready || !xtensa_trap_ready) {
            klog("PANIC: XT-3.4 ownership checks failed\n");
            for (;;)
                __asm__ volatile ("waiti 15");
        }

        {
            uint32_t intenable;
            __asm__ volatile("rsr %0, intenable" : "=a"(intenable));
            if (intenable != XTENSA_TIMER0_INTMASK) {
                klogf("XT-3.4: normalize INTENABLE %lx -> %lx\n",
                      (unsigned long)intenable, (unsigned long)XTENSA_TIMER0_INTMASK);
                __asm__ volatile("wsr %0, intenable; rsync" ::
                                 "a"(XTENSA_TIMER0_INTMASK));
            }
        }
    }
}

void target_post_mount(void)
{
#ifdef PPAP_TESTS
    ktest_run_all();
#endif
}

const char *target_init_path(void)
{
    /* CC-1: no user-mode binaries — skip PID 1 launch.
     * kmain() will enter the idle loop directly. */
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
    return "xtensa_cc";
}

uint32_t target_caps(void)
{
    /* CC-1: no capabilities enabled yet.
     * Future: TARGET_CAP_SPI | TARGET_CAP_DISPLAY | TARGET_CAP_KBD */
    return 0;
}
