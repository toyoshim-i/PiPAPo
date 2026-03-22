/*
 * xtensa_cc_stubs.c — Stub symbols for ESP-IDF build
 *
 * The PPAP kernel expects linker symbols (__romfs_start, __romfs_end,
 * __bss_end, __stack_top) that are defined by custom linker scripts on
 * other targets.  ESP-IDF owns the linker script, so we provide C-level
 * stubs here.  These will be replaced with proper ESP-IDF linker
 * fragments when romfs and memory layout are fully integrated.
 */

#include <stdint.h>

/* No embedded romfs yet — make start == end so kmain() skips mount. */
const uint8_t __romfs_start[0] = {};
const uint8_t __romfs_end[0] = {};

/* mm_init() uses these to compute kernel memory usage.
 * Provide reasonable values for the ESP32-S3 SRAM layout.
 * These are approximate — the real values come from ESP-IDF's heap. */
char __bss_end = 0;
char __stack_top = 0;
