/*
 * probe_ram.h — m68k bus-error catching RAM probe
 *
 * Returns the number of bytes accessible starting from `start`, up to
 * `max_size`.  Implemented in probe_ram.S; used by the m68k mem_helper
 * override to size the runtime page pool.
 */

#ifndef PPAP_ARCH_M68K_KERNEL_CORE_PROBE_RAM_H
#define PPAP_ARCH_M68K_KERNEL_CORE_PROBE_RAM_H

#include <stdint.h>

uint32_t m68k_probe_ram(uint32_t start, uint32_t max_size);

#endif /* PPAP_ARCH_M68K_KERNEL_CORE_PROBE_RAM_H */
