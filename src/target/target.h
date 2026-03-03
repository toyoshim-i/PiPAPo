/*
 * target.h — Target abstraction API
 *
 * Each build target (qemu_arm, pico1, pico1calc) implements the functions
 * declared here.  The kernel calls these hooks instead of directly invoking
 * hardware-specific functions, keeping shared code free of #ifdef guards.
 *
 * Link-time selection: each target provides the same symbols via its own
 * target_<name>.c; CMake links the correct one.  No vtable indirection.
 *
 * This is the *only* header shared kernel code includes for target-specific
 * behaviour.  Individual target headers (pico1.h, picocalc.h) are included
 * only from their own target_*.c files.
 */

#ifndef PPAP_TARGET_H
#define PPAP_TARGET_H

#include <stdint.h>

/* Target capability flags — returned by target_caps() */
#define TARGET_CAP_SD       (1u << 0)   /* Has SD card slot            */
#define TARGET_CAP_SPI      (1u << 1)   /* Has SPI bus for peripherals */
#define TARGET_CAP_CORE1    (1u << 2)   /* Dual-core (Core 1 usable)   */
#define TARGET_CAP_REALUART (1u << 3)   /* PL011 UART (not CMSDK)      */

/*
 * target_early_init() — called first in kmain(), before mm_init().
 *
 * Responsibilities (target-dependent):
 *   - UART console init (so uart_puts() works immediately)
 *   - Clock PLL init (RP2040 targets only)
 *   - UART baud rate update after PLL
 *   - SPI bus init (PicoCalc only)
 *
 * After this call: UART console is operational, system clock is final.
 */
void target_early_init(void);

/*
 * target_late_init() — called after VFS/blkdev init, before fstab mount.
 *
 * Responsibilities (target-dependent):
 *   - SD card detection and initialization
 *   - Block device registration (mmcblk0)
 *   - UART switch to IRQ mode
 *   - MPU configuration
 *   - Core 1 launch
 *   - RAM block device setup (QEMU only)
 *
 * After this call: all block devices are registered, ready for fstab mount.
 */
void target_late_init(void);

/*
 * target_post_mount() — called after VFS + fstab mount, before sched_start().
 *
 * Default build (PPAP_TESTS off): empty (no-op).
 * Test build  (PPAP_TESTS on):   runs kernel integration tests via
 *                                 ktest_run_all().
 *
 * Each target implements this with #ifdef PPAP_TESTS in its own .c file.
 */
void target_post_mount(void);

/*
 * target_init_path() — returns the path to exec as PID 1.
 *
 * Default build (PPAP_TESTS off): "/sbin/init" (busybox ash shell).
 * Test build  (PPAP_TESTS on):   "/bin/runtests" (automated test runner).
 *
 * Each target implements this with #ifdef PPAP_TESTS in its own .c file.
 */
const char *target_init_path(void);

/*
 * target_caps() — returns a bitmask of TARGET_CAP_* flags.
 *
 * Used by shared code to conditionally skip SD-dependent steps
 * (e.g., fstab skips VFAT/loopback entries when TARGET_CAP_SD is absent).
 */
uint32_t target_caps(void);

#endif /* PPAP_TARGET_H */
