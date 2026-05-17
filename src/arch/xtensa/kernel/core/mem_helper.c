/*
 * arch/xtensa/.../mem_helper.c — Xtensa overrides for mm_helper hooks
 *
 * ESP32-S3 layout PPAP cooperates with:
 *
 *   page pool   — internal DRAM, allocated from ESP-IDF's heap_caps
 *                 (MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL).
 *   ram_text    — internal IRAM, allocated from heap_caps with
 *                 MALLOC_CAP_EXEC.  Holds user binary text.
 *   ext_text    — PSRAM, optional.  Same role as ram_text when
 *                 SPIRAM_XIP is enabled.
 *   ext_rodata  — PSRAM, optional.  Holds user binary rodata.
 *
 * SRAM1 on the S3 is mapped at BOTH a DRAM (0x3FC8_8000) and an IRAM
 * (0x4037_C000) address.  ESP-IDF exposes the two views as
 * independent heap regions, so an EXEC allocation (ram_text) and an
 * 8BIT|INTERNAL allocation (page pool) can hand out overlapping
 * physical bytes — any page-pool zero then corrupts user text loaded
 * in ram_text.  The pool-sizing loop here rejects any grab whose DRAM
 * range, projected to IRAM, intersects the ram_text arena.
 */

#include "kernel/core/mm/mem_helper.h"

#include "common/errno.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/common/spinlock.h"
#include "kernel/core/mm/page.h"
#include "sdkconfig.h"

#ifndef MEM_REGION_RAM_TEXT_ARENA_SIZE
#define MEM_REGION_RAM_TEXT_ARENA_SIZE (64u * 1024u)
#endif
#ifndef MEM_REGION_EXT_TEXT_ARENA_SIZE
#define MEM_REGION_EXT_TEXT_ARENA_SIZE (512u * 1024u)
#endif
#ifndef MEM_REGION_EXT_RODATA_ARENA_SIZE
#define MEM_REGION_EXT_RODATA_ARENA_SIZE (256u * 1024u)
#endif

#define MEM_REGION_ALIGN 16u
#define MEM_REGION_FREE_MAX 16u

/* SRAM1 dual-mapping constants — same physical SRAM, two address views. */
#define SRAM1_DRAM_BASE 0x3FC88000u
#define SRAM1_DRAM_END 0x3FCE0000u  /* exclusive */
#define SRAM1_IRAM_OFFS 0x00704000u /* DRAM_addr + offs = IRAM_alias_addr */

/* Keep at least this much internal-DRAM free for ESP-IDF after the
 * page-pool grab.  Smaller leaves IDF's pipe / UART / FreeRTOS task
 * allocations starved; the test_pipe wedge that motivated this code
 * lived just below 64 KB free.  96 KB matches the empirically-proven
 * safe leftover with a margin for future kernel growth. */
#define XTENSA_IDF_HEAP_RESERVE (96u * 1024u)

/* ── Linear arena allocator (shared by ram_text / ext_text / ext_rodata)  */

typedef struct {
  uint8_t *base;
  uint32_t size;
} arena_block_t;

typedef struct {
  void *raw;
  uint8_t *base;
  uint32_t size;
  arena_block_t free[MEM_REGION_FREE_MAX];
  uint32_t free_count;
  uint8_t ready;
} arena_t;

static arena_t ram_text_arena;
static arena_t ext_text_arena;
static arena_t ext_rodata_arena;

/* Page-pool sizing reported by mem_helper_init_pool — surfaced via
 * mem_helper_log_state so the late-boot banner can show what mm_init
 * chose even on targets whose monitor missed the early output. */
static uintptr_t s_pool_base;
static uint32_t s_pool_pages;
static uint32_t s_idf_free_before_kb;
static uint32_t s_idf_free_after_kb;

static uint32_t align_up(uint32_t size) {
  return (size + MEM_REGION_ALIGN - 1u) & ~(MEM_REGION_ALIGN - 1u);
}

static int arena_init(arena_t *arena, uint32_t arena_size, uint32_t caps,
                      const char *name) {
  uint32_t want = arena_size;

  if (arena->ready) return 0;

  /* Auto-downsize: requested size may exceed ESP-IDF's largest
   * contiguous free block (especially for IRAM-only or internal-only
   * DRAM).  Halve until heap_caps_malloc succeeds or we fall below a
   * one-page minimum. */
  while (want >= PAGE_SIZE) {
    arena->raw = heap_caps_malloc(want + MEM_REGION_ALIGN - 1u, caps);
    if (arena->raw) break;
    want /= 2u;
  }
  if (!arena->raw) return -(int)ENOMEM;

  uintptr_t raw = (uintptr_t)arena->raw;
  uintptr_t aligned =
      (raw + MEM_REGION_ALIGN - 1u) & ~(uintptr_t)(MEM_REGION_ALIGN - 1u);
  uintptr_t raw_end = raw + want + MEM_REGION_ALIGN - 1u;

  arena->base = (uint8_t *)aligned;
  arena->size = (uint32_t)(raw_end - aligned);
  if (arena->size > want) arena->size = want;

  arena->free[0].base = arena->base;
  arena->free[0].size = arena->size;
  arena->free_count = 1u;
  arena->ready = 1u;

  if (want < arena_size) {
    mod_vfs.klogf("MM:   %s %lx-%lx  %lu KB reserved (requested %lu KB)\n",
                  name, (unsigned long)(uintptr_t)arena->base,
                  (unsigned long)(uintptr_t)(arena->base + arena->size - 1u),
                  (unsigned long)(arena->size / 1024u),
                  (unsigned long)(arena_size / 1024u));
  } else {
    mod_vfs.klogf("MM:   %s %lx-%lx  %lu KB reserved\n", name,
                  (unsigned long)(uintptr_t)arena->base,
                  (unsigned long)(uintptr_t)(arena->base + arena->size - 1u),
                  (unsigned long)(arena->size / 1024u));
  }
  return 0;
}

static int arena_alloc(arena_t *arena, proc_image_segment_t *seg, uint32_t size,
                       ppap_mem_class_t mem_class, uint32_t flags) {
  uint32_t need = align_up(size);

  if (!arena->ready) return -(int)ENOMEM;

  uint32_t saved = spin_lock_irqsave(SPIN_MEM);
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

static void arena_free(arena_t *arena, const proc_image_segment_t *seg,
                       const char *name) {
  uint8_t *base = (uint8_t *)seg->base;
  uint32_t size = align_up(seg->size);
  uint8_t *end = base + size;

  if (!arena->ready || !base || size == 0) return;

  uint32_t saved = spin_lock_irqsave(SPIN_MEM);
  uint32_t pos = 0;
  while (pos < arena->free_count && arena->free[pos].base < base) pos++;

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
    mod_vfs.klogf("MM: %s free-list overflow\n", name);
    return;
  }

  for (uint32_t i = arena->free_count; i > pos; i--)
    arena->free[i] = arena->free[i - 1];
  arena->free[pos].base = base;
  arena->free[pos].size = size;
  arena->free_count++;
  spin_unlock_irqrestore(SPIN_MEM, saved);
}

static int arena_contains(const arena_t *arena, const void *ptr) {
  if (!arena->ready || !ptr) return 0;
  uintptr_t a = (uintptr_t)arena->base;
  uintptr_t p = (uintptr_t)ptr;
  return p >= a && p < a + arena->size;
}

static uint32_t arena_total_bytes(const arena_t *arena) {
  return arena->ready ? arena->size : 0u;
}

static uint32_t arena_free_bytes(const arena_t *arena) {
  if (!arena->ready) return 0u;
  uint32_t free_bytes = 0u;
  for (uint32_t i = 0; i < arena->free_count; i++)
    free_bytes += arena->free[i].size;
  return free_bytes;
}

static uint32_t arena_largest_free(const arena_t *arena) {
  if (!arena->ready) return 0u;
  uint32_t largest = 0u;
  for (uint32_t i = 0; i < arena->free_count; i++)
    if (arena->free[i].size > largest) largest = arena->free[i].size;
  return largest;
}

/* ── Hook overrides ──────────────────────────────────────────────────────── */

int mem_helper_init_arenas(void) {
  int err = arena_init(&ram_text_arena, MEM_REGION_RAM_TEXT_ARENA_SIZE,
                       MALLOC_CAP_EXEC, "ram_text");
  if (err < 0) {
    mod_vfs.klogf("MM: ram_text reservation failed (%d)\n", err);
    return err;
  }

#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
  uint32_t psram_total = (uint32_t)esp_psram_get_size();
  if (psram_total == 0) {
    mod_vfs.klogf("MM:   psram unavailable; external arenas disabled\n");
    return 0;
  }
  uint32_t psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  mod_vfs.klogf("MM:   psram total %lu KB, free %lu KB before reservation\n",
                (unsigned long)(psram_total / 1024u),
                (unsigned long)(psram_free / 1024u));

  err = arena_init(&ext_text_arena, MEM_REGION_EXT_TEXT_ARENA_SIZE,
                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, "ext_text");
  if (err < 0) return err;
  err = arena_init(&ext_rodata_arena, MEM_REGION_EXT_RODATA_ARENA_SIZE,
                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, "ext_rodata");
  if (err < 0) return err;

#if defined(CONFIG_SPIRAM_XIP_FROM_PSRAM) && CONFIG_SPIRAM_XIP_FROM_PSRAM
  mod_vfs.klogf("MM:   psram xip enabled for staged text/rodata\n");
#else
  mod_vfs.klogf("MM:   psram xip disabled; external arenas are staging-only\n");
#endif

  psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  mod_vfs.klogf("MM:   psram free %lu KB after reservation\n",
                (unsigned long)(psram_free / 1024u));
#else
  mod_vfs.klogf("MM:   psram support disabled; external arenas unavailable\n");
#endif
  return 0;
}

int mem_helper_init_pool(uintptr_t *base_out) {
  const uint32_t caps = MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL;
  uint32_t free_before = (uint32_t)heap_caps_get_free_size(caps);
  uint32_t want = PAGE_COUNT_MAX;
  void *pool = (void *)0;

  for (;;) {
    if (want < 1u) break;
    uint32_t bytes = want * PAGE_SIZE + PAGE_SIZE; /* +1 for alignment slack */
    pool = heap_caps_malloc(bytes, caps);
    if (!pool) {
      want /= 2u;
      continue;
    }
    /* Reject any pool that (a) physically aliases ram_text via SRAM1's
     * dual mapping, or (b) leaves IDF's internal-DRAM heap below the
     * reserved working set. */
    uintptr_t pool_lo = (uintptr_t)pool;
    uintptr_t pool_hi = pool_lo + bytes;
    int aliases = mem_helper_pool_aliases_text(pool_lo, pool_hi);
    uint32_t free_now = (uint32_t)heap_caps_get_free_size(caps);
    if (!aliases && (free_now >= XTENSA_IDF_HEAP_RESERVE || want <= 1u)) break;
    heap_caps_free(pool);
    pool = (void *)0;
    want /= 2u;
  }
  if (!pool) return -(int)ENOMEM;

  uintptr_t raw = (uintptr_t)pool;
  uintptr_t aligned = (raw + PAGE_SIZE - 1u) & ~((uintptr_t)PAGE_SIZE - 1u);
  uintptr_t usable = (raw + want * PAGE_SIZE + PAGE_SIZE) - aligned;
  uint32_t pages = usable / PAGE_SIZE;
  if (pages > want) pages = want;

  s_pool_base = aligned;
  s_pool_pages = pages;
  s_idf_free_before_kb = free_before / 1024u;
  s_idf_free_after_kb = (uint32_t)heap_caps_get_free_size(caps) / 1024u;

  *base_out = aligned;
  page_count = pages; /* shared global in page.c */
  return 0;
}

int mem_helper_alloc(ppap_mem_class_t mem_class, uint32_t size, uint32_t flags,
                     proc_image_segment_t *out) {
  switch (mem_class) {
    case PPAP_MEM_RAM_TEXT:
      /* User text must be IRAM-accessible; the generic DRAM page pool
       * can't satisfy that.  Prefer ext_text (PSRAM, larger) when
       * available, fall back to ram_text. */
      if (ext_text_arena.ready) {
        int rc = arena_alloc(&ext_text_arena, out, size, mem_class, flags);
        if (rc == 0) return 0;
      }
      return arena_alloc(&ram_text_arena, out, size, mem_class, flags);
    case PPAP_MEM_EXT_TEXT:
      return arena_alloc(&ext_text_arena, out, size, mem_class, flags);
    case PPAP_MEM_EXT_RODATA:
      return arena_alloc(&ext_rodata_arena, out, size, mem_class, flags);
    default:
      return -(int)ENOSYS;
  }
}

int mem_helper_free(const proc_image_segment_t *seg) {
  switch (seg->mem_class) {
    case PPAP_MEM_RAM_TEXT:
      /* May have come from either ext_text (PSRAM) or ram_text (IRAM). */
      if (arena_contains(&ext_text_arena, seg->base))
        arena_free(&ext_text_arena, seg, "ext_text(ram_text)");
      else
        arena_free(&ram_text_arena, seg, "ram_text");
      return 0;
    case PPAP_MEM_EXT_TEXT:
      arena_free(&ext_text_arena, seg, "ext_text");
      return 0;
    case PPAP_MEM_EXT_RODATA:
      arena_free(&ext_rodata_arena, seg, "ext_rodata");
      return 0;
    default:
      return -(int)ENOSYS;
  }
}

int32_t mem_helper_total_bytes(ppap_mem_class_t cls) {
  switch (cls) {
    case PPAP_MEM_RAM_TEXT:
      return (int32_t)arena_total_bytes(&ram_text_arena);
    case PPAP_MEM_EXT_TEXT:
      return (int32_t)arena_total_bytes(&ext_text_arena);
    case PPAP_MEM_EXT_RODATA:
      return (int32_t)arena_total_bytes(&ext_rodata_arena);
    default:
      return -1;
  }
}

int32_t mem_helper_free_bytes(ppap_mem_class_t cls) {
  switch (cls) {
    case PPAP_MEM_RAM_TEXT:
      return (int32_t)arena_free_bytes(&ram_text_arena);
    case PPAP_MEM_EXT_TEXT:
      return (int32_t)arena_free_bytes(&ext_text_arena);
    case PPAP_MEM_EXT_RODATA:
      return (int32_t)arena_free_bytes(&ext_rodata_arena);
    default:
      return -1;
  }
}

int32_t mem_helper_largest_free(ppap_mem_class_t cls) {
  switch (cls) {
    case PPAP_MEM_RAM_TEXT:
      return (int32_t)arena_largest_free(&ram_text_arena);
    case PPAP_MEM_EXT_TEXT:
      return (int32_t)arena_largest_free(&ext_text_arena);
    case PPAP_MEM_EXT_RODATA:
      return (int32_t)arena_largest_free(&ext_rodata_arena);
    default:
      return -1;
  }
}

int mem_helper_pool_aliases_text(uintptr_t pool_lo, uintptr_t pool_hi) {
  if (!ram_text_arena.ready) return 0;
  if (pool_lo >= SRAM1_DRAM_END || pool_hi <= SRAM1_DRAM_BASE) return 0;
  uintptr_t clip_lo = pool_lo > SRAM1_DRAM_BASE ? pool_lo : SRAM1_DRAM_BASE;
  uintptr_t clip_hi = pool_hi < SRAM1_DRAM_END ? pool_hi : SRAM1_DRAM_END;
  uintptr_t iram_lo = clip_lo + SRAM1_IRAM_OFFS;
  uintptr_t iram_hi = clip_hi + SRAM1_IRAM_OFFS;
  uintptr_t rt_lo = (uintptr_t)ram_text_arena.base;
  uintptr_t rt_hi = rt_lo + ram_text_arena.size;
  return (iram_lo < rt_hi && iram_hi > rt_lo) ? 1 : 0;
}

static void log_arena(const char *name, const arena_t *arena) {
  if (!arena->ready) {
    mod_vfs.klogf("MM:   %s arena: not ready\n", name);
    return;
  }
  uintptr_t lo = (uintptr_t)arena->base;
  uintptr_t hi = lo + arena->size - 1u;
  mod_vfs.klogf("MM:   %s arena %lx-%lx (%lu KB)\n", name, (unsigned long)lo,
                (unsigned long)hi, (unsigned long)(arena->size / 1024u));
}

void mem_helper_log_state(void) {
  /* Page-pool sizing + IDF heap before/after + alias status. */
  uintptr_t pool_hi = s_pool_base + (uintptr_t)s_pool_pages * PAGE_SIZE;
  int aliases = mem_helper_pool_aliases_text(s_pool_base, pool_hi);
  mod_vfs.klogf(
      "MM:   pool %lu pages @ %lx; IDF internal-DRAM %lu->%lu KB; "
      "ram_text alias=%s\n",
      (unsigned long)s_pool_pages, (unsigned long)s_pool_base,
      (unsigned long)s_idf_free_before_kb, (unsigned long)s_idf_free_after_kb,
      aliases ? "YES" : "no");

  log_arena("ram_text   ", &ram_text_arena);
  log_arena("ext_text   ", &ext_text_arena);
  log_arena("ext_rodata ", &ext_rodata_arena);
}
