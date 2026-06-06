/*
 * target_default.c — Weak default implementations of optional target hooks.
 *
 * Each function here is a weak symbol that can be overridden by a target's
 * target_<name>.c.  Targets that do not need a particular hook simply inherit
 * the do-nothing default defined here.
 */

#include "target.h"

/*
 * Default target_mount_rootfs(): no target-specific rootfs.
 * Overridden by targets that load a rootfs from an external source
 * (e.g. x68k loads a UFS image from RAM placed there by stage2).
 */
__attribute__((weak)) int target_mount_rootfs(void) { return -1; }

__attribute__((weak)) void ktest_run_all(void) {}

__attribute__((weak)) void target_post_mount(void) { ktest_run_all(); }

__attribute__((weak)) const char *target_init_path(void) {
  return PPAP_DEFAULT_INIT_PATH;
}

__attribute__((weak)) void target_enable_deferred_timer(void) {}

__attribute__((weak)) void target_idle_poll(void) {}

__attribute__((weak)) void target_may_poweroff(uint8_t s) { (void)s; }

__attribute__((weak)) uint32_t target_debug_hwbp_slots(void) { return 0; }

__attribute__((weak)) int target_debug_hwbp_set(uint32_t slot, uint32_t addr) {
  (void)slot;
  (void)addr;
  return -1;
}

__attribute__((weak)) int target_debug_hwbp_clear(uint32_t slot) {
  (void)slot;
  return -1;
}
