/*
 * mem_class.h — Memory class enum
 *
 * Classifies which arena / pool a piece of memory comes from.  Used by
 * region_alloc dispatch and the arch-level mem_helper hooks.  This is
 * an mm-layer concept; proc-image descriptors live in proc_image.h.
 */

#ifndef PPAP_KERNEL_COMMON_CORE_MEM_CLASS_H
#define PPAP_KERNEL_COMMON_CORE_MEM_CLASS_H

typedef enum {
  PPAP_MEM_NONE = 0,
  PPAP_MEM_RAM_TEXT,
  PPAP_MEM_RAM_RODATA,
  PPAP_MEM_RAM_DATA,
  PPAP_MEM_EXT_TEXT,
  PPAP_MEM_EXT_RODATA,
  PPAP_MEM_ROM_TEXT,
  PPAP_MEM_ROM_RODATA,
  PPAP_MEM_RAM_STACK,
  PPAP_MEM_DEVICE_DMA,
} ppap_mem_class_t;

#endif /* PPAP_KERNEL_COMMON_CORE_MEM_CLASS_H */
