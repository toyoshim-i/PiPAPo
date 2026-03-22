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

#include "../target.h"
#include "xtensa_cc.h"
#include "klog.h"

/* ESP32-S3 SYSTIMER registers — used to disable FreeRTOS tick alarms
 * before PPAP takes over.  Base: 0x6001F000 (TRM §11.5). */
#define SYSTIMER_BASE           0x6001F000u
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

void target_early_init(void)
{
    /* ESP-IDF has already initialized:
     *   - System clock (240 MHz PLL)
     *   - UART0 console (115200 8N1 via USB CDC or UART pins)
     *   - Flash cache (XIP)
     *   - GPIO subsystem
     *
     * Disable ESP-IDF's SYSTIMER alarms and silence all pending
     * interrupts.  FreeRTOS sets up timer tick + other interrupt sources;
     * without clearing them, they fire as "Unhandled interrupt N". */
    SYSTIMER_INT_ENA_REG = 0;          /* disable all 3 alarm interrupts */
    SYSTIMER_INT_CLR_REG = 0x7u;       /* clear any pending alarm flags  */

    /* Disable all interrupts at the Xtensa core level.
     * INTENABLE controls which interrupts the CPU will take. */
    __asm__ volatile("wsr %0, intenable; rsync" :: "r"(0));

    klog("PiPAPo booting... [xtensa_cc]\n");
    klogf("System clock: %u MHz\n", PPAP_SYS_HZ / 1000000u);
}

void target_late_init(void)
{
    /* CC-1: no SD, no display, no keyboard, no timer yet.
     * CC-2 will add timer init here.
     * CC-4/CC-5/CC-6 will add display, keyboard, SD. */
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
    return NULL;  /* no init process yet */
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
