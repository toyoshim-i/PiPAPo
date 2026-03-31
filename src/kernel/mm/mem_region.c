/*
 * mem_region.c — Memory-region allocation helpers
 */

#include "mem_region.h"

#include "kernel/common/errno.h"
#include "kernel/common/spinlock.h"
#include "kernel/klog.h"
#include "page.h"

#if defined(__xtensa__)
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "sdkconfig.h"
#endif

static uint32_t mem_region_page_count(uint32_t size) {
  return (size + PAGE_SIZE - 1u) / PAGE_SIZE;
}

static uint32_t mem_region_page_pool_total_bytes(void) {
  return page_count * PAGE_SIZE;
}

static uint32_t mem_region_page_pool_free_bytes(void) {
  return page_free_count() * PAGE_SIZE;
}

static uint32_t mem_region_page_pool_largest_free_bytes(void) {
  return page_max_contiguous() * PAGE_SIZE;
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

#ifndef MEM_REGION_EXT_TEXT_ARENA_SIZE
#define MEM_REGION_EXT_TEXT_ARENA_SIZE (512u * 1024u)
#endif

#ifndef MEM_REGION_EXT_RODATA_ARENA_SIZE
#define MEM_REGION_EXT_RODATA_ARENA_SIZE (256u * 1024u)
#endif

#define MEM_REGION_FREE_MAX 16u
#define MEM_REGION_RAM_DATA_PAGES_MAX \
  (MEM_REGION_RAM_DATA_ARENA_SIZE / PAGE_SIZE)

typedef struct {
  uint8_t *base;
  uint32_t size;
} mem_region_block_t;

typedef struct {
  void *raw;
  uint8_t *base;
  uint32_t size;
  mem_region_block_t free[MEM_REGION_FREE_MAX];
  uint32_t free_count;
  uint8_t ready;
} mem_region_linear_arena_t;

static mem_region_linear_arena_t ram_text_arena;
static mem_region_linear_arena_t ext_text_arena;
static mem_region_linear_arena_t ext_rodata_arena;
static void *ram_data_arena_raw;
static uint8_t *ram_data_arena_base;
static uint32_t ram_data_page_count;
static uint8_t ram_data_page_used[MEM_REGION_RAM_DATA_PAGES_MAX];
static uint8_t ram_data_ready;

static int mem_region_linear_arena_init(mem_region_linear_arena_t *arena,
                                        uint32_t arena_size, uint32_t caps,
                                        const char *name) {
  uint32_t arena_bytes = arena_size + MEM_REGION_ALIGN - 1u;
  uintptr_t raw;
  uintptr_t aligned;
  uintptr_t raw_end;

  if (arena->ready) return 0;

  arena->raw = heap_caps_malloc(arena_bytes, caps);
  if (!arena->raw) return -(int)ENOMEM;

  raw = (uintptr_t)arena->raw;
  aligned = (raw + MEM_REGION_ALIGN - 1u) & ~(uintptr_t)(MEM_REGION_ALIGN - 1u);
  raw_end = raw + arena_bytes;

  arena->base = (uint8_t *)aligned;
  arena->size = (uint32_t)(raw_end - aligned);
  if (arena->size > arena_size) arena->size = arena_size;

  arena->free[0].base = arena->base;
  arena->free[0].size = arena->size;
  arena->free_count = 1u;
  arena->ready = 1u;

  klogf("MM:   %s %lx-%lx  %lu KB reserved\n", name,
        (unsigned long)(uintptr_t)arena->base,
        (unsigned long)(uintptr_t)(arena->base + arena->size - 1u),
        (unsigned long)(arena->size / 1024u));
  return 0;
}

static int mem_region_xtensa_text_init(void) {
  return mem_region_linear_arena_init(&ram_text_arena,
                                      MEM_REGION_RAM_TEXT_ARENA_SIZE,
                                      MALLOC_CAP_EXEC, "ram_text");
}

static int mem_region_xtensa_data_init(void) {
  uint32_t arena_bytes = MEM_REGION_RAM_DATA_ARENA_SIZE + PAGE_SIZE - 1u;
  uintptr_t raw;
  uintptr_t aligned;
  uintptr_t raw_end;
  uintptr_t usable;

  if (ram_data_ready) return 0;

  ram_data_arena_raw = heap_caps_malloc(arena_bytes, MALLOC_CAP_8BIT);
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

  klogf("MM:   ram_data %lx-%lx  %lu KB reserved\n",
        (unsigned long)(uintptr_t)ram_data_arena_base,
        (unsigned long)(uintptr_t)(ram_data_arena_base +
                              ram_data_page_count * PAGE_SIZE - 1u),
        (unsigned long)((ram_data_page_count * PAGE_SIZE) / 1024u));
  return 0;
}

static int mem_region_xtensa_psram_init(void) {
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
  uint32_t total_size;
  uint32_t free_size;
  int err;

  total_size = (uint32_t)esp_psram_get_size();
  if (total_size == 0) {
    klog("MM:   psram unavailable; external arenas disabled\n");
    return 0;
  }

  free_size = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  klogf("MM:   psram total %lu KB, free %lu KB before reservation\n",
        (unsigned long)(total_size / 1024u), (unsigned long)(free_size / 1024u));

  err = mem_region_linear_arena_init(&ext_text_arena,
                                     MEM_REGION_EXT_TEXT_ARENA_SIZE,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                     "ext_text");
  if (err < 0) return err;

  err = mem_region_linear_arena_init(&ext_rodata_arena,
                                     MEM_REGION_EXT_RODATA_ARENA_SIZE,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                     "ext_rodata");
  if (err < 0) return err;

#if defined(CONFIG_SPIRAM_XIP_FROM_PSRAM) && CONFIG_SPIRAM_XIP_FROM_PSRAM
  klog("MM:   psram xip enabled for staged text/rodata\n");
#else
  klog("MM:   psram xip disabled; external arenas are staging-only\n");
#endif

  if (ext_text_arena.ready) {
    klogf("MM:   ext_text executable=%lu byte_access=%lu\n",
          (unsigned long)(uint32_t)esp_ptr_executable(ext_text_arena.base),
          (unsigned long)(uint32_t)esp_ptr_byte_accessible(ext_text_arena.base));
  }
  if (ext_rodata_arena.ready) {
    klogf("MM:   ext_rodata executable=%lu byte_access=%lu\n",
          (unsigned long)(uint32_t)esp_ptr_executable(ext_rodata_arena.base),
          (unsigned long)(uint32_t)esp_ptr_byte_accessible(ext_rodata_arena.base));
  }

  free_size = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  klogf("MM:   psram free %lu KB after reservation\n", (unsigned long)(free_size / 1024u));
  return 0;
#else
  klog("MM:   psram support disabled; external arenas unavailable\n");
  return 0;
#endif
}

static int mem_region_ram_data_contains(const void *base, uint32_t size) {
  uintptr_t addr = (uintptr_t)base;
  uintptr_t start = (uintptr_t)ram_data_arena_base;
  uintptr_t end = start + ram_data_page_count * PAGE_SIZE;
  uintptr_t last = addr + size;

  if (!ram_data_ready || !base || size == 0) return 0;
  return addr >= start && last <= end;
}

static uint32_t mem_region_linear_arena_total_bytes(
    const mem_region_linear_arena_t *arena) {
  if (!arena->ready) return 0u;
  return arena->size;
}

static uint32_t mem_region_linear_arena_free_bytes(
    const mem_region_linear_arena_t *arena) {
  uint32_t free_bytes = 0u;

  if (!arena->ready) return 0u;
  for (uint32_t i = 0; i < arena->free_count; i++)
    free_bytes += arena->free[i].size;
  return free_bytes;
}

static uint32_t mem_region_linear_arena_largest_free_bytes(
    const mem_region_linear_arena_t *arena) {
  uint32_t largest = 0u;

  if (!arena->ready) return 0u;
  for (uint32_t i = 0; i < arena->free_count; i++) {
    if (arena->free[i].size > largest) largest = arena->free[i].size;
  }
  return largest;
}

static uint32_t mem_region_ram_data_total_bytes(void) {
  if (!ram_data_ready) return 0u;
  return ram_data_page_count * PAGE_SIZE;
}

static uint32_t mem_region_ram_data_free_bytes(void) {
  uint32_t free_pages = 0u;

  if (!ram_data_ready) return 0u;
  for (uint32_t i = 0; i < ram_data_page_count; i++) {
    if (!ram_data_page_used[i]) free_pages++;
  }
  return free_pages * PAGE_SIZE;
}

static uint32_t mem_region_ram_data_largest_free_bytes(void) {
  uint32_t run = 0u;
  uint32_t largest = 0u;

  if (!ram_data_ready) return 0u;
  for (uint32_t i = 0; i < ram_data_page_count; i++) {
    if (!ram_data_page_used[i]) {
      run++;
      if (run > largest) largest = run;
    } else {
      run = 0u;
    }
  }
  return largest * PAGE_SIZE;
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

static int mem_region_linear_arena_alloc(mem_region_linear_arena_t *arena,
                                         proc_image_segment_t *seg,
                                         uint32_t size,
                                         ppap_mem_class_t mem_class,
                                         uint32_t flags) {
  uint32_t saved;
  uint32_t need = mem_region_align(size);

  if (!arena->ready) return -(int)ENOMEM;

  saved = spin_lock_irqsave(SPIN_MEM);
  for (uint32_t i = 0; i < arena->free_count; i++) {
    if (arena->free[i].size < need) continue;

    uint8_t *base = arena->free[i].base;
    arena->free[i].base += need;
    arena->free[i].size -= need;
    if (arena->free[i].size == 0) {
      for (uint32_t j = i + 1; j < arena->free_count; j++)
        arena->free[j - 1] = arena->free[j];
      arena->free_count--;
    }
    spin_unlock_irqrestore(SPIN_MEM, saved);
    *seg = proc_image_segment_make(base, size, mem_class, flags);
    return 0;
  }
  spin_unlock_irqrestore(SPIN_MEM, saved);
  return -(int)ENOMEM;
}

static int mem_region_alloc_ram_text(proc_image_segment_t *seg, uint32_t size,
                                     uint32_t flags) {
  /* Try PSRAM first (larger, cheaper), fall back to IRAM.
   * The loader doesn't know or care which one it gets — both are
   * executable on ESP32-S3 when SPIRAM_XIP is enabled. */
  if (ext_text_arena.ready) {
    int rc = mem_region_linear_arena_alloc(&ext_text_arena, seg, size,
                                           PPAP_MEM_RAM_TEXT, flags);
    if (rc == 0) return 0;
  }
  return mem_region_linear_arena_alloc(&ram_text_arena, seg, size,
                                       PPAP_MEM_RAM_TEXT, flags);
}

static int mem_region_alloc_ext_text(proc_image_segment_t *seg, uint32_t size,
                                     uint32_t flags) {
  return mem_region_linear_arena_alloc(&ext_text_arena, seg, size,
                                       PPAP_MEM_EXT_TEXT, flags);
}

static int mem_region_alloc_ext_rodata(proc_image_segment_t *seg, uint32_t size,
                                       uint32_t flags) {
  return mem_region_linear_arena_alloc(&ext_rodata_arena, seg, size,
                                       PPAP_MEM_EXT_RODATA, flags);
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
  seg->base_page = mm_ptr_to_page(base);
  return 0;
}

static void mem_region_linear_arena_free(mem_region_linear_arena_t *arena,
                                         const proc_image_segment_t *seg,
                                         const char *name) {
  uint32_t saved;
  uint8_t *base = (uint8_t *)seg->base;
  uint32_t size = mem_region_align(seg->size);
  uint8_t *end = base + size;
  uint32_t pos = 0;

  if (!arena->ready || !base || size == 0) return;

  saved = spin_lock_irqsave(SPIN_MEM);
  while (pos < arena->free_count && arena->free[pos].base < base)
    pos++;

  if (pos > 0 &&
      arena->free[pos - 1].base + arena->free[pos - 1].size == base) {
    arena->free[pos - 1].size += size;
    if (pos < arena->free_count &&
        arena->free[pos - 1].base + arena->free[pos - 1].size ==
            arena->free[pos].base) {
      arena->free[pos - 1].size += arena->free[pos].size;
      for (uint32_t i = pos + 1; i < arena->free_count; i++)
        arena->free[i - 1] = arena->free[i];
      arena->free_count--;
    }
    spin_unlock_irqrestore(SPIN_MEM, saved);
    return;
  }

  if (pos < arena->free_count && end == arena->free[pos].base) {
    arena->free[pos].base = base;
    arena->free[pos].size += size;
    spin_unlock_irqrestore(SPIN_MEM, saved);
    return;
  }

  if (arena->free_count >= MEM_REGION_FREE_MAX) {
    spin_unlock_irqrestore(SPIN_MEM, saved);
    klogf("MM: %s free-list overflow\n", name);
    return;
  }

  for (uint32_t i = arena->free_count; i > pos; i--)
    arena->free[i] = arena->free[i - 1];
  arena->free[pos].base = base;
  arena->free[pos].size = size;
  arena->free_count++;
  spin_unlock_irqrestore(SPIN_MEM, saved);
}

static int mem_region_arena_contains(const mem_region_linear_arena_t *arena,
                                     const void *ptr) {
  if (!arena->ready || !ptr) return 0;
  uintptr_t a = (uintptr_t)arena->base;
  uintptr_t p = (uintptr_t)ptr;
  return p >= a && p < a + arena->size;
}

static void mem_region_free_ram_text(const proc_image_segment_t *seg) {
  /* May have been allocated from PSRAM (ext_text) or IRAM (ram_text) */
  if (mem_region_arena_contains(&ext_text_arena, seg->base))
    mem_region_linear_arena_free(&ext_text_arena, seg, "ext_text(ram_text)");
  else
    mem_region_linear_arena_free(&ram_text_arena, seg, "ram_text");
}

static void mem_region_free_ext_text(const proc_image_segment_t *seg) {
  mem_region_linear_arena_free(&ext_text_arena, seg, "ext_text");
}

static void mem_region_free_ext_rodata(const proc_image_segment_t *seg) {
  mem_region_linear_arena_free(&ext_rodata_arena, seg, "ext_rodata");
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
  err = mem_region_xtensa_psram_init();
  if (err < 0) {
    klogf("MM: mem_region_init: psram reservation failed (%d)\n", err);
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
  seg->base_page = mm_ptr_to_page(base);
  return 0;
}

int mem_region_alloc(proc_image_segment_t *seg, ppap_mem_class_t mem_class,
                     uint32_t size, uint32_t flags) {
  if (!seg) return -(int)EINVAL;

  switch (mem_class) {
#if defined(__xtensa__)
    case PPAP_MEM_EXT_TEXT:
      return mem_region_alloc_ext_text(seg, size, flags);
    case PPAP_MEM_EXT_RODATA:
      return mem_region_alloc_ext_rodata(seg, size, flags);
#endif
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
    seg->base_page = mm_ptr_to_page(base);
    return 0;
  }

  return -(int)EINVAL;
}

void mem_region_free(const proc_image_segment_t *seg) {
  uint32_t n_pages;

  if (!seg || !seg->base || seg->size == 0) return;

  switch (seg->mem_class) {
#if defined(__xtensa__)
    case PPAP_MEM_EXT_TEXT:
      mem_region_free_ext_text(seg);
      return;
    case PPAP_MEM_EXT_RODATA:
      mem_region_free_ext_rodata(seg);
      return;
#endif
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

void mem_region_free_tracked_page_id(page_id_t id) {
  if (id == PAGE_ID_INVALID) return;
  mm_page_free(id);
}

/* ── Page-index wrappers ────────────────────────────────────────────── */

page_id_t mem_region_page_alloc(void) { return mm_page_alloc(); }

page_id_t mem_region_page_alloc_contiguous(uint32_t n_pages) {
  return mm_page_alloc_contiguous(n_pages);
}

void mem_region_page_free(page_id_t id) { mm_page_free(id); }

uint32_t mem_region_page_linear(page_id_t id) {
  return mm_page_linear(id);
}

page_id_t mem_region_ptr_to_page(void *ptr) {
  return mm_ptr_to_page(ptr);
}

#if !defined(__ia16__)
void *mem_region_page_to_ptr(page_id_t id) {
  return mm_page_to_ptr(id);
}
#endif

void mem_region_page_read(page_id_t id, uint16_t off,
                          void *buf, uint16_t len) {
  mm_page_read(id, off, buf, len);
}

void mem_region_page_write(page_id_t id, uint16_t off,
                           const void *buf, uint16_t len) {
  mm_page_write(id, off, buf, len);
}

/* ── Capacity queries ──────────────────────────────────────────────── */

uint32_t mem_region_total_bytes(ppap_mem_class_t mem_class) {
  switch (mem_class) {
#if defined(__xtensa__)
    case PPAP_MEM_RAM_TEXT:
      return mem_region_linear_arena_total_bytes(&ram_text_arena);
    case PPAP_MEM_RAM_DATA:
      return mem_region_ram_data_total_bytes();
    case PPAP_MEM_EXT_TEXT:
      return mem_region_linear_arena_total_bytes(&ext_text_arena);
    case PPAP_MEM_EXT_RODATA:
      return mem_region_linear_arena_total_bytes(&ext_rodata_arena);
#endif
    case PPAP_MEM_RAM_RODATA:
    case PPAP_MEM_RAM_STACK:
    case PPAP_MEM_DEVICE_DMA:
      return mem_region_page_pool_total_bytes();
#if !defined(__xtensa__)
    case PPAP_MEM_RAM_DATA:
    case PPAP_MEM_RAM_TEXT:
      return mem_region_page_pool_total_bytes();
#endif
    case PPAP_MEM_NONE:
    case PPAP_MEM_ROM_TEXT:
    case PPAP_MEM_ROM_RODATA:
    default:
      return 0u;
  }
}

uint32_t mem_region_free_bytes(ppap_mem_class_t mem_class) {
  switch (mem_class) {
#if defined(__xtensa__)
    case PPAP_MEM_RAM_TEXT:
      return mem_region_linear_arena_free_bytes(&ram_text_arena);
    case PPAP_MEM_RAM_DATA:
      return mem_region_ram_data_free_bytes();
    case PPAP_MEM_EXT_TEXT:
      return mem_region_linear_arena_free_bytes(&ext_text_arena);
    case PPAP_MEM_EXT_RODATA:
      return mem_region_linear_arena_free_bytes(&ext_rodata_arena);
#endif
    case PPAP_MEM_RAM_RODATA:
    case PPAP_MEM_RAM_STACK:
    case PPAP_MEM_DEVICE_DMA:
      return mem_region_page_pool_free_bytes();
#if !defined(__xtensa__)
    case PPAP_MEM_RAM_DATA:
    case PPAP_MEM_RAM_TEXT:
      return mem_region_page_pool_free_bytes();
#endif
    case PPAP_MEM_NONE:
    case PPAP_MEM_ROM_TEXT:
    case PPAP_MEM_ROM_RODATA:
    default:
      return 0u;
  }
}

uint32_t mem_region_largest_free_bytes(ppap_mem_class_t mem_class) {
  switch (mem_class) {
#if defined(__xtensa__)
    case PPAP_MEM_RAM_TEXT:
      return mem_region_linear_arena_largest_free_bytes(&ram_text_arena);
    case PPAP_MEM_RAM_DATA:
      return mem_region_ram_data_largest_free_bytes();
    case PPAP_MEM_EXT_TEXT:
      return mem_region_linear_arena_largest_free_bytes(&ext_text_arena);
    case PPAP_MEM_EXT_RODATA:
      return mem_region_linear_arena_largest_free_bytes(&ext_rodata_arena);
#endif
    case PPAP_MEM_RAM_RODATA:
    case PPAP_MEM_RAM_STACK:
    case PPAP_MEM_DEVICE_DMA:
      return mem_region_page_pool_largest_free_bytes();
#if !defined(__xtensa__)
    case PPAP_MEM_RAM_DATA:
    case PPAP_MEM_RAM_TEXT:
      return mem_region_page_pool_largest_free_bytes();
#endif
    case PPAP_MEM_NONE:
    case PPAP_MEM_ROM_TEXT:
    case PPAP_MEM_ROM_RODATA:
    default:
      return 0u;
  }
}
