/*
 * target/pcxt/.../mem_helper.c — Page-pool sizing via BIOS INT 12h
 *
 * INT 12h returns the conventional memory size in KB (AX).  We cap at
 * RAM_END (default = end of the 640 KB conventional region) and clamp
 * to PAGE_COUNT_MAX before publishing page_count.  Lives at target
 * level because INT 12h is a PC-BIOS service; a hypothetical non-PC
 * i16 target would supply its own probe.
 */

#include "kernel/core/mm/mem_helper.h"

#include "kernel/common/config.h"
#include "kernel/common/core/page_types.h"

int mem_helper_init_pool(uint32_t *base_out) {
  uint16_t kb;
  __asm__ volatile("int $0x12" : "=a"(kb) : : "cc");
  uint32_t ram_top = (uint32_t)kb * 1024u;
  if (ram_top > (uint32_t)RAM_END) ram_top = (uint32_t)RAM_END;
  uint32_t pages = 0;
  if (ram_top > (uint32_t)PAGE_POOL_BASE) {
    pages = (ram_top - (uint32_t)PAGE_POOL_BASE) / PAGE_SIZE;
    if (pages > PAGE_COUNT_MAX) pages = PAGE_COUNT_MAX;
  }
  page_count = pages;
  *base_out = PAGE_POOL_BASE;
  return 0;
}
