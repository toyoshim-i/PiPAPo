/*
 * mem_region.c — Memory-region allocation helpers
 */

#include "mem_region.h"

#include "kernel/common/errno.h"
#include "kernel/common/spinlock.h"
#include "kernel/klog.h"
#include "page.h"

static uint32_t mem_region_page_count(uint32_t size) {
  return (size + PAGE_SIZE - 1u) / PAGE_SIZE;
}

#if defined(__xtensa__)
#define MEM_REGION_ALIGN 16u

static uint32_t mem_region_align(uint32_t size) {
  return (size + MEM_REGION_ALIGN - 1u) & ~(MEM_REGION_ALIGN - 1u);
}

#ifndef MEM_REGION_RAM_TEXT_ARENA_SIZE
#define MEM_REGION_RAM_TEXT_ARENA_SIZE (64u * 1024u)
#endif

#define MEM_REGION_FREE_MAX 16u

typedef struct {
  uint8_t *base;
  uint32_t size;
} mem_region_block_t;

static void *ram_text_arena_raw;
static uint8_t *ram_text_arena_base;
static uint32_t ram_text_arena_size;
static mem_region_block_t ram_text_free[MEM_REGION_FREE_MAX];
static uint32_t ram_text_free_count;
static uint8_t ram_text_ready;

static int mem_region_xtensa_text_init(void) {
  extern void *heap_caps_malloc(unsigned int size, uint32_t caps);

  uint32_t arena_bytes = MEM_REGION_RAM_TEXT_ARENA_SIZE + MEM_REGION_ALIGN - 1u;
  uintptr_t raw;
  uintptr_t aligned;
  uintptr_t raw_end;

  if (ram_text_ready) return 0;

  ram_text_arena_raw = heap_caps_malloc(arena_bytes, (1u << 0));
  if (!ram_text_arena_raw) return -(int)ENOMEM;

  raw = (uintptr_t)ram_text_arena_raw;
  aligned = (raw + MEM_REGION_ALIGN - 1u) & ~(uintptr_t)(MEM_REGION_ALIGN - 1u);
  raw_end = raw + arena_bytes;

  ram_text_arena_base = (uint8_t *)aligned;
  ram_text_arena_size = (uint32_t)(raw_end - aligned);
  if (ram_text_arena_size > MEM_REGION_RAM_TEXT_ARENA_SIZE)
    ram_text_arena_size = MEM_REGION_RAM_TEXT_ARENA_SIZE;

  ram_text_free[0].base = ram_text_arena_base;
  ram_text_free[0].size = ram_text_arena_size;
  ram_text_free_count = 1u;
  ram_text_ready = 1u;

  klogf("MM:   ram_text %x-%x  %u KB reserved\n",
        (uint32_t)(uintptr_t)ram_text_arena_base,
        (uint32_t)(uintptr_t)(ram_text_arena_base + ram_text_arena_size - 1u),
        ram_text_arena_size / 1024u);
  return 0;
}

static int mem_region_alloc_ram_text(proc_image_segment_t *seg, uint32_t size,
                                     uint32_t flags) {
  uint32_t saved;
  uint32_t need = mem_region_align(size);

  if (!ram_text_ready) return -(int)ENOMEM;

  saved = spin_lock_irqsave(SPIN_MEM);
  for (uint32_t i = 0; i < ram_text_free_count; i++) {
    if (ram_text_free[i].size < need) continue;

    uint8_t *base = ram_text_free[i].base;
    ram_text_free[i].base += need;
    ram_text_free[i].size -= need;
    if (ram_text_free[i].size == 0) {
      for (uint32_t j = i + 1; j < ram_text_free_count; j++)
        ram_text_free[j - 1] = ram_text_free[j];
      ram_text_free_count--;
    }
    spin_unlock_irqrestore(SPIN_MEM, saved);
    *seg = proc_image_segment_make(base, size, PPAP_MEM_RAM_TEXT, flags);
    return 0;
  }
  spin_unlock_irqrestore(SPIN_MEM, saved);
  return -(int)ENOMEM;
}

static void mem_region_free_ram_text(const proc_image_segment_t *seg) {
  uint32_t saved;
  uint8_t *base = (uint8_t *)seg->base;
  uint32_t size = mem_region_align(seg->size);
  uint8_t *end = base + size;
  uint32_t pos = 0;

  if (!ram_text_ready || !base || size == 0) return;

  saved = spin_lock_irqsave(SPIN_MEM);
  while (pos < ram_text_free_count && ram_text_free[pos].base < base)
    pos++;

  if (pos > 0 &&
      ram_text_free[pos - 1].base + ram_text_free[pos - 1].size == base) {
    ram_text_free[pos - 1].size += size;
    if (pos < ram_text_free_count &&
        ram_text_free[pos - 1].base + ram_text_free[pos - 1].size ==
            ram_text_free[pos].base) {
      ram_text_free[pos - 1].size += ram_text_free[pos].size;
      for (uint32_t i = pos + 1; i < ram_text_free_count; i++)
        ram_text_free[i - 1] = ram_text_free[i];
      ram_text_free_count--;
    }
    spin_unlock_irqrestore(SPIN_MEM, saved);
    return;
  }

  if (pos < ram_text_free_count && end == ram_text_free[pos].base) {
    ram_text_free[pos].base = base;
    ram_text_free[pos].size += size;
    spin_unlock_irqrestore(SPIN_MEM, saved);
    return;
  }

  if (ram_text_free_count >= MEM_REGION_FREE_MAX) {
    spin_unlock_irqrestore(SPIN_MEM, saved);
    klog("MM: ram_text free-list overflow\n");
    return;
  }

  for (uint32_t i = ram_text_free_count; i > pos; i--)
    ram_text_free[i] = ram_text_free[i - 1];
  ram_text_free[pos].base = base;
  ram_text_free[pos].size = size;
  ram_text_free_count++;
  spin_unlock_irqrestore(SPIN_MEM, saved);
}
#endif

int mem_region_init(void) {
#if defined(__xtensa__)
  return mem_region_xtensa_text_init();
#else
  return 0;
#endif
}

static int mem_region_alloc_page_backed(proc_image_segment_t *seg,
                                        ppap_mem_class_t mem_class,
                                        uint32_t size, uint32_t flags) {
  uint32_t n_pages;
  void *base;

  if (size == 0) {
    *seg = (proc_image_segment_t){0};
    return 0;
  }

  n_pages = mem_region_page_count(size);
  if (n_pages == 1) {
    base = page_alloc();
  } else {
    base = page_alloc_contiguous(n_pages);
  }
  if (!base) return -(int)ENOMEM;

  *seg = proc_image_segment_make(base, size, mem_class, flags);
  return 0;
}

int mem_region_alloc(proc_image_segment_t *seg, ppap_mem_class_t mem_class,
                     uint32_t size, uint32_t flags) {
  if (!seg) return -(int)EINVAL;

  switch (mem_class) {
    case PPAP_MEM_RAM_RODATA:
    case PPAP_MEM_RAM_DATA:
    case PPAP_MEM_RAM_STACK:
    case PPAP_MEM_DEVICE_DMA:
      return mem_region_alloc_page_backed(seg, mem_class, size, flags);

#if defined(__xtensa__)
    case PPAP_MEM_RAM_TEXT:
      return mem_region_alloc_ram_text(seg, size, flags);
#else
    case PPAP_MEM_RAM_TEXT:
      return mem_region_alloc_page_backed(seg, mem_class, size, flags);
#endif

    case PPAP_MEM_NONE:
    case PPAP_MEM_ROM_TEXT:
    case PPAP_MEM_ROM_RODATA:
    default:
      return -(int)EINVAL;
  }
}

void mem_region_free(const proc_image_segment_t *seg) {
  uint32_t n_pages;

  if (!seg || !seg->base || seg->size == 0) return;

  switch (seg->mem_class) {
    case PPAP_MEM_RAM_RODATA:
    case PPAP_MEM_RAM_DATA:
    case PPAP_MEM_RAM_STACK:
    case PPAP_MEM_DEVICE_DMA:
      n_pages = mem_region_page_count(seg->size);
      for (uint32_t i = 0; i < n_pages; i++)
        page_free((uint8_t *)seg->base + i * PAGE_SIZE);
      return;

#if defined(__xtensa__)
    case PPAP_MEM_RAM_TEXT:
      mem_region_free_ram_text(seg);
      return;
#else
    case PPAP_MEM_RAM_TEXT:
      n_pages = mem_region_page_count(seg->size);
      for (uint32_t i = 0; i < n_pages; i++)
        page_free((uint8_t *)seg->base + i * PAGE_SIZE);
      return;
#endif

    case PPAP_MEM_NONE:
    case PPAP_MEM_ROM_TEXT:
    case PPAP_MEM_ROM_RODATA:
    default:
      return;
  }
}
