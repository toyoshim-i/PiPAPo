/*
 * pico1calc_caps.h -- Runtime-detected PicoCalc capability flags
 *
 * The PicoCalc keyboard / LCD expansion board is optional.  The VFS
 * side probes for the STM32 keyboard controller during kbd_init() and
 * writes the result into pico1calc_has_kbd; the core side reads it
 * from target_caps() without needing to include a VFS driver header.
 *
 * Lives under kernel/common/ because the VFS module writes it and the
 * core module reads it; a function exported through mod_vfs would be a
 * heavier interface than the one-bit state actually needs.
 */

#ifndef PPAP_TARGET_PICO1CALC_KERNEL_COMMON_PICO1CALC_CAPS_H
#define PPAP_TARGET_PICO1CALC_KERNEL_COMMON_PICO1CALC_CAPS_H

#include <stdbool.h>

/* false until VFS_EVENT_PLL_CHANGED completes; true afterwards iff
 * the STM32 keyboard controller responded during kbd_init(). */
extern bool pico1calc_has_kbd;

#endif /* PPAP_TARGET_PICO1CALC_KERNEL_COMMON_PICO1CALC_CAPS_H */
