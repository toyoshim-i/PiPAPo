/*
 * arch/m68k/.../mem_helper.c — m68k page-pool sizing
 *
 * Probes physical RAM via m68k_probe_ram (bus-error catching) starting
 * at PAGE_POOL_BASE and clamps to PAGE_COUNT_MAX.  RAM_END is the hard
 * ceiling — qemu_m68k uses the default (PAGE_POOL_BASE + PAGE_POOL_SIZE);
 * x68k overrides it to 0xC00000 to stop before VRAM.
 */

#include "kernel/core/mm/mem_helper.h"

#include "kernel/common/config.h"
#include "kernel/common/core/page_types.h"
#include "kernel/core/probe_ram.h"

int mem_helper_init_pool(uint32_t *base_out) {
  uint32_t max_bytes = (uint32_t)RAM_END - (uint32_t)PAGE_POOL_BASE;
  uint32_t ram_bytes = m68k_probe_ram(PAGE_POOL_BASE, max_bytes);
  if (ram_bytes > max_bytes) ram_bytes = max_bytes;
  uint32_t pages = ram_bytes / PAGE_SIZE;
  if (pages > PAGE_COUNT_MAX) pages = PAGE_COUNT_MAX;
  page_count = pages;
  *base_out = PAGE_POOL_BASE;
  return 0;
}
