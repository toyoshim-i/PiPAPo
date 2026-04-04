/*
 * xtensa_cc_stubs.c — Stub symbols for ESP-IDF build
 *
 * The PPAP kernel expects linker symbols (__bss_end, __stack_top) that are
 * defined by custom linker scripts on other targets.  ESP-IDF owns the
 * linker script, so we provide C-level stubs here.
 *
 * __romfs_start / __romfs_end are provided by romfs_data.S (.incbin).
 */

#include <stdint.h>

/* mm_init() uses these to compute kernel memory usage.
 * Provide reasonable values for the ESP32-S3 SRAM layout.
 * These are approximate — the real values come from ESP-IDF's heap. */
char __bss_end = 0;
char __stack_top = 0;
