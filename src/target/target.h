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

#ifndef PPAP_TARGET_TARGET_H
#define PPAP_TARGET_TARGET_H

#include <stdint.h>

/* Target capability flags — returned by target_caps() */
#define TARGET_CAP_SD (1u << 0)       /* Has SD card slot            */
#define TARGET_CAP_SPI (1u << 1)      /* Has SPI bus for peripherals */
#define TARGET_CAP_CORE1 (1u << 2)    /* Dual-core (Core 1 usable)   */
#define TARGET_CAP_REALUART (1u << 3) /* PL011 UART (not CMSDK)      */
#define TARGET_CAP_DISPLAY (1u << 4)  /* LCD display (fbcon)         */
#define TARGET_CAP_KBD (1u << 5)      /* Keyboard controller         */

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
 * Default build (PPAP_TESTS off): empty (weak ktest_run_all() no-op).
 * Test build  (PPAP_TESTS on):   runs kernel integration tests when the real
 *                                 ktest_run_all() object is linked.
 *
 * The common weak implementation in target_default.c applies this policy for
 * all targets.  Targets may override when they intentionally skip kernel tests
 * or need target-specific post-mount work.
 */
void target_post_mount(void);

/*
 * target_init_path() — returns the path to exec as PID 1.
 *
 * Default build (PPAP_TESTS off): "/sbin/init" (busybox ash shell).
 * Test build  (PPAP_TESTS on):   "/bin/runtests" (automated test runner).
 * Extended test build
 * (PPAP_TESTS_EXTENDED on):      "/bin/runtests_ext" (extended test runner).
 *
 * target_default.c returns PPAP_DEFAULT_INIT_PATH, supplied by the build for
 * all targets.  Targets may override only when they need a different PID 1.
 */
const char *target_init_path(void);

/*
 * target_name() — returns a short target identifier string.
 *
 * Used by sys_uname() for the nodename field, allowing user-space to
 * distinguish hardware variants (e.g. "pico1", "pico1calc", "qemu_arm").
 */
const char *target_name(void);

/*
 * target_caps() — returns a bitmask of TARGET_CAP_* flags.
 *
 * Used by shared code to conditionally skip SD-dependent steps
 * (e.g., fstab skips VFAT/loopback entries when TARGET_CAP_SD is absent).
 */
uint32_t target_caps(void);

/*
 * target_mount_rootfs() — mount the root filesystem (optional hook).
 *
 * Called from kmain() when no embedded romfs is present (__romfs_start ==
 * __romfs_end).  The target registers its rootfs block device and mounts it
 * at "/".
 *
 * Returns 0 on success, negative on failure.
 * Default weak implementation returns -1 (no target-specific rootfs).
 */
int target_mount_rootfs(void);

/*
 * target_enable_deferred_timer() — start any target timer deferred until
 * user-space bring-up reaches a safe point.
 *
 * Most targets do not need this and inherit the default no-op.
 */
void target_enable_deferred_timer(void);

/*
 * target_idle_poll() — target-specific idle-loop work.
 *
 * Called from sched_idle_poll() after VFS_EVENT_IDLE has been fired.
 * Override for target-specific work that is not covered by VFS event
 * handling.  Default weak implementation is a no-op.
 */
void target_idle_poll(void);

/*
 * target_may_poweroff() — request QEMU to exit.
 *
 * Writes to an architecture-specific QEMU exit device (isa-debug-exit,
 * sifive_test, semihosting, etc.).  status 0 = clean shutdown,
 * status 1 = panic/fault.
 *
 * Default weak implementation is a no-op (hardware targets).
 */
void target_may_poweroff(uint8_t status);

/*
 * Optional native debugger HW-breakpoint hooks.
 *
 * These are used by ptrace for native real-surface breakpoints on targets
 * that expose hardware breakpoint comparators to software.
 *
 * Default weak implementations return "unsupported" in target_default.c.
 */
uint32_t target_debug_hwbp_slots(void);
int target_debug_hwbp_set(uint32_t slot, uint32_t addr);
int target_debug_hwbp_clear(uint32_t slot);

#endif /* PPAP_TARGET_TARGET_H */
