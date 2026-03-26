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

#ifndef MEM_REGION_RAM_DATA_ARENA_SIZE
#define MEM_REGION_RAM_DATA_ARENA_SIZE (128u * 1024u)
#endif

#define MEM_REGION_FREE_MAX 16u
#define MEM_REGION_RAM_DATA_PAGES_MAX \
  (MEM_REGION_RAM_DATA_ARENA_SIZE / PAGE_SIZE)

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
static void *ram_data_arena_raw;
static uint8_t *ram_data_arena_base;
static uint32_t ram_data_page_count;
static uint8_t ram_data_page_used[MEM_REGION_RAM_DATA_PAGES_MAX];
static uint8_t ram_data_ready;

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

static int mem_region_xtensa_data_init(void) {
  extern void *heap_caps_malloc(unsigned int size, uint32_t caps);

  uint32_t arena_bytes = MEM_REGION_RAM_DATA_ARENA_SIZE + PAGE_SIZE - 1u;
  uintptr_t raw;
  uintptr_t aligned;
  uintptr_t raw_end;
  uintptr_t usable;

  if (ram_data_ready) return 0;

  ram_data_arena_raw = heap_caps_malloc(arena_bytes, (1u << 2));
  if (!ram_data_arena_raw) return -(int)ENOMEM;

  raw = (uintptr_t)ram_data_arena_raw;
  aligned = (raw + PAGE_SIZE - 1u) & ~((uintptr_t)PAGE_SIZE - 1u);
  raw_end = raw + arena_bytes;
  usable = raw_end - aligned;

  ram_data_arena_base = (uint8_t *)aligned;
  ram_data_page_count = (uint32_t)(usable / PAGE_SIZE);
  if (ram_data_page_count > MEM_REGION_RAM_DATA_PAGES_MAX)
    ram_data_page_count = MEM_REGION_RAM_DATA_PAGES_MAX;

  for (uint32_t i = 0; i < ram_data_page_count; i++)
    ram_data_page_used[i] = 0u;
  ram_data_ready = 1u;

  klogf("MM:   ram_data %x-%x  %u KB reserved\n",
        (uint32_t)(uintptr_t)ram_data_arena_base,
        (uint32_t)(uintptr_t)(ram_data_arena_base +
                              ram_data_page_count * PAGE_SIZE - 1u),
        (ram_data_page_count * PAGE_SIZE) / 1024u);
  return 0;
}

static int mem_region_ram_data_contains(const void *base, uint32_t size) {
  uintptr_t addr = (uintptr_t)base;
  uintptr_t start = (uintptr_t)ram_data_arena_base;
  uintptr_t end = start + ram_data_page_count * PAGE_SIZE;
  uintptr_t last = addr + size;

  if (!ram_data_ready || !base || size == 0) return 0;
  return addr >= start && last <= end;
}

static int mem_region_try_mark_ram_data(uint32_t start_page,
                                        uint32_t n_pages) {
  if (start_page > ram_data_page_count ||
      n_pages > ram_data_page_count - start_page)
    return 0;
  for (uint32_t i = 0; i < n_pages; i++) {
    if (ram_data_page_used[start_page + i]) return 0;
  }
  for (uint32_t i = 0; i < n_pages; i++)
    ram_data_page_used[start_page + i] = 1u;
  return 1;
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

static int mem_region_alloc_ram_data(proc_image_segment_t *seg, uint32_t size,
                                     uint32_t flags) {
  uint32_t saved;
  uint32_t n_pages = mem_region_page_count(size);

  if (!ram_data_ready) return -(int)ENOMEM;
  if (size == 0) {
    *seg = (proc_image_segment_t){0};
    return 0;
  }

  saved = spin_lock_irqsave(SPIN_MEM);
  for (uint32_t i = 0; i + n_pages <= ram_data_page_count; i++) {
    if (!mem_region_try_mark_ram_data(i, n_pages)) continue;
    spin_unlock_irqrestore(SPIN_MEM, saved);
    *seg = proc_image_segment_make(ram_data_arena_base + i * PAGE_SIZE, size,
                                   PPAP_MEM_RAM_DATA, flags);
    return 0;
  }
  spin_unlock_irqrestore(SPIN_MEM, saved);
  return -(int)ENOMEM;
}

static int mem_region_alloc_ram_data_at(proc_image_segment_t *seg, void *base,
                                        uint32_t size, uint32_t flags) {
  uint32_t saved;
  uintptr_t start;
  uint32_t start_page;
  uint32_t n_pages = mem_region_page_count(size);

  if (!ram_data_ready) return -(int)ENOMEM;
  if (!base || size == 0) return -(int)EINVAL;
  if (((uintptr_t)base & (PAGE_SIZE - 1u)) != 0) return -(int)EINVAL;
  if (!mem_region_ram_data_contains(base, n_pages * PAGE_SIZE))
    return -(int)EINVAL;

  start = (uintptr_t)base - (uintptr_t)ram_data_arena_base;
  start_page = (uint32_t)(start / PAGE_SIZE);

  saved = spin_lock_irqsave(SPIN_MEM);
  if (!mem_region_try_mark_ram_data(start_page, n_pages)) {
    spin_unlock_irqrestore(SPIN_MEM, saved);
    return -(int)ENOMEM;
  }
  spin_unlock_irqrestore(SPIN_MEM, saved);

  *seg = proc_image_segment_make(base, size, PPAP_MEM_RAM_DATA, flags);
  return 0;
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

static void mem_region_free_ram_data(const proc_image_segment_t *seg) {
  uint32_t saved;
  uintptr_t start;
  uint32_t start_page;
  uint32_t n_pages;

  if (!seg || !seg->base || seg->size == 0) return;
  n_pages = mem_region_page_count(seg->size);
  if (!mem_region_ram_data_contains(seg->base, n_pages * PAGE_SIZE)) return;

  start = (uintptr_t)seg->base - (uintptr_t)ram_data_arena_base;
  start_page = (uint32_t)(start / PAGE_SIZE);

  saved = spin_lock_irqsave(SPIN_MEM);
  for (uint32_t i = 0; i < n_pages && start_page + i < ram_data_page_count;
       i++) {
    ram_data_page_used[start_page + i] = 0u;
  }
  spin_unlock_irqrestore(SPIN_MEM, saved);
}
#endif

int mem_region_init(void) {
#if defined(__xtensa__)
  int err = mem_region_xtensa_text_init();
  if (err < 0) {
    klogf("MM: mem_region_init: ram_text reservation failed (%d)\n", err);
    return err;
  }
  err = mem_region_xtensa_data_init();
  if (err < 0) {
    klogf("MM: mem_region_init: ram_data reservation failed (%d)\n", err);
    return err;
  }
  return 0;
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
    case PPAP_MEM_RAM_STACK:
    case PPAP_MEM_DEVICE_DMA:
      return mem_region_alloc_page_backed(seg, mem_class, size, flags);

#if defined(__xtensa__)
    case PPAP_MEM_RAM_DATA:
      return mem_region_alloc_ram_data(seg, size, flags);
    case PPAP_MEM_RAM_TEXT:
      return mem_region_alloc_ram_text(seg, size, flags);
#else
    case PPAP_MEM_RAM_DATA:
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

int mem_region_alloc_at(proc_image_segment_t *seg, ppap_mem_class_t mem_class,
                        void *base, uint32_t size, uint32_t flags) {
  if (!seg || !base || size == 0) return -(int)EINVAL;

#if defined(__xtensa__)
  if (mem_class == PPAP_MEM_RAM_DATA)
    return mem_region_alloc_ram_data_at(seg, base, size, flags);
#endif

  if (mem_class == PPAP_MEM_RAM_DATA || mem_class == PPAP_MEM_RAM_STACK ||
      mem_class == PPAP_MEM_RAM_RODATA ||
      mem_class == PPAP_MEM_DEVICE_DMA) {
    uint32_t n_pages = mem_region_page_count(size);
    for (uint32_t i = 0; i < n_pages; i++) {
      void *addr = (uint8_t *)base + i * PAGE_SIZE;
      if (!page_alloc_at(addr)) {
        for (uint32_t j = 0; j < i; j++)
          page_free((uint8_t *)base + j * PAGE_SIZE);
        return -(int)ENOMEM;
      }
    }
    *seg = proc_image_segment_make(base, size, mem_class, flags);
    return 0;
  }

  return -(int)EINVAL;
}

void mem_region_free(const proc_image_segment_t *seg) {
  uint32_t n_pages;

  if (!seg || !seg->base || seg->size == 0) return;

  switch (seg->mem_class) {
    case PPAP_MEM_RAM_RODATA:
    case PPAP_MEM_RAM_STACK:
    case PPAP_MEM_DEVICE_DMA:
      n_pages = mem_region_page_count(seg->size);
      for (uint32_t i = 0; i < n_pages; i++)
        page_free((uint8_t *)seg->base + i * PAGE_SIZE);
      return;

#if defined(__xtensa__)
    case PPAP_MEM_RAM_DATA:
      mem_region_free_ram_data(seg);
      return;
    case PPAP_MEM_RAM_TEXT:
      mem_region_free_ram_text(seg);
      return;
#else
    case PPAP_MEM_RAM_DATA:
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

void mem_region_free_tracked_page(void *page) {
#if defined(__xtensa__)
  proc_image_segment_t seg;

  if (mem_region_ram_data_contains(page, PAGE_SIZE)) {
    seg = proc_image_segment_make(page, PAGE_SIZE, PPAP_MEM_RAM_DATA,
                                  PROC_IMAGE_SEG_WRITABLE);
    mem_region_free_ram_data(&seg);
    return;
  }
#endif

  page_free(page);
}
