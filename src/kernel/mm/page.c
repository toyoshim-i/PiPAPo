/*
 * page.c — Physical page allocator
 *
 * Free-stack implementation: a fixed array of PAGE_COUNT_MAX page_id_t indices,
 * managed as a LIFO stack.  Alloc pops the top; free pushes onto the top.
 * Both operations are O(1) with no heap or dynamic storage needed.
 * Using page indices (not void*) allows pages beyond the 16-bit address
 * range on i16, where uintptr_t is only 16 bits but RAM extends to 640 KB.
 *
 * mm_init() also prints the boot-time memory map, showing:
 *   - actual kernel data usage (measured from __bss_end at link time)
 *   - page pool, I/O buffer, and DMA/Core1 region sizes
 * This lets you track how much kernel data grows as Phase 1+ code is added.
 */

#include "page.h"

#include "kmem.h"
#if !defined(__m68k__)
#include "xip.h"
#endif
#include <stddef.h>

#include "../klog.h"
#include "../common/spinlock.h"

/* ── Linker-provided symbols ────────────────────────────────────────────────
 */
/*
 * These are section boundary symbols defined by the linker script.
 * In C, a linker symbol `foo` is accessed as `&foo` — the *address* of the
 * symbol IS its value (i.e. the memory address it marks).
 *
 * __bss_end:  first byte after the .bss section (= first free byte of
 *             kernel data region after zeroing).
 * __stack_top: top of the initial kernel stack (= end of RAM_KERNEL region).
 */
extern char __bss_end;
extern char __stack_top;

/* ── Free-stack state ───────────────────────────────────────────────────────
 */

static page_id_t free_stack[PAGE_COUNT_MAX];
static uint32_t free_top = 0u; /* index of next empty slot (0 = pool empty) */
uint32_t page_count = 0u;      /* runtime page count (set by mm_init)       */
uint32_t oom_count = 0u;       /* number of page_alloc() failures */

/* Linear address table: maps page_id → 32-bit linear address.
 * Populated by mm_init(); used by mm_page_to_ptr, mm_page_read/write,
 * and page_alloc (to convert index back to void*). */
static uint32_t page_linear[PAGE_COUNT_MAX];

#if defined(__m68k__)
/* Assembly RAM probe — detects accessible RAM via bus error catching.
 * Returns total accessible bytes starting from `start`, up to `max_size`. */
extern uint32_t m68k_probe_ram(uint32_t start, uint32_t max_size);
#elif defined(__ia16__)
/* BIOS INT 12h: returns conventional memory size in KB (AX). */
static uint32_t i16_detect_ram(void) {
  uint16_t kb;
  __asm__ volatile("int $0x12" : "=a"(kb) : : "cc");
  return (uint32_t)kb * 1024u;
}
#endif

/* ── Internal helpers ───────────────────────────────────────────────────────
 */

/* ── Public API ─────────────────────────────────────────────────────────────
 */

/* On Xtensa, the page pool is allocated from ESP-IDF's heap at runtime
 * (see mm_init).  This variable stores the base address. */
#if defined(__xtensa__)
static uintptr_t xtensa_pool_base;
#endif

void mm_init(void) {
  uintptr_t bss_end = (uintptr_t)&__bss_end;
  uintptr_t stack_top = (uintptr_t)&__stack_top;
  uintptr_t kern_used = bss_end - SRAM_KERNEL_BASE;

  /* Detect available RAM and set runtime page_count.
   * On m68k: probe RAM via bus error catching (two-phase: 1MB then 4KB).
   * On ARM:  fixed RAM size, use compile-time PAGE_COUNT_MAX directly. */
#if defined(__m68k__)
  {
    uint32_t probe_end = RAM_END;
    uint32_t max_bytes = probe_end - PAGE_POOL_BASE;
    uint32_t ram_bytes = m68k_probe_ram(PAGE_POOL_BASE, max_bytes);
    if (ram_bytes > max_bytes) ram_bytes = max_bytes;
    page_count = ram_bytes / PAGE_SIZE;
    if (page_count > PAGE_COUNT_MAX) page_count = PAGE_COUNT_MAX;
  }
#elif defined(__xtensa__)
  /* ESP-IDF owns the DRAM heap.  Allocate page pool from ESP-IDF so
   * both systems agree on who owns what memory.  Use MALLOC_CAP_8BIT
   * (cap 2) to get byte-accessible DRAM, NOT IRAM. */
  {
    extern void *heap_caps_malloc(unsigned int size, uint32_t caps);
    uint32_t pool_bytes = PAGE_COUNT_MAX * PAGE_SIZE + PAGE_SIZE; /* +1 page for alignment */
    void *pool = heap_caps_malloc(pool_bytes, (1u << 2)); /* MALLOC_CAP_8BIT */
    if (!pool) {
      klog("MM: FATAL — heap_caps_malloc failed for page pool\n");
      for (;;) __asm__ volatile("waiti 15");
    }
    /* Align pool start to page boundary */
    uintptr_t raw = (uintptr_t)pool;
    xtensa_pool_base = (raw + PAGE_SIZE - 1u) & ~((uintptr_t)PAGE_SIZE - 1u);
    /* Recalculate how many pages fit after alignment */
    uintptr_t usable = (raw + pool_bytes) - xtensa_pool_base;
    page_count = usable / PAGE_SIZE;
    if (page_count > PAGE_COUNT_MAX) page_count = PAGE_COUNT_MAX;
  }
#elif defined(__ia16__)
  {
    uint32_t ram_top = i16_detect_ram();
    if (ram_top > RAM_END) ram_top = RAM_END;
    uint32_t pool_start = (uint32_t)PAGE_POOL_BASE;
    if (ram_top > pool_start) {
      page_count = (uint32_t)(ram_top - pool_start) / PAGE_SIZE;
      if (page_count > PAGE_COUNT_MAX) page_count = PAGE_COUNT_MAX;
    } else {
      page_count = 0;
    }
  }
#else
  page_count = PAGE_COUNT_MAX;
#endif

  /* Build the free stack: push pages from the pool that don't overlap
   * with the kernel stack.  On QEMU (flat memory model) the initial
   * stack may extend into the page pool region.  Skip those pages. */
  uintptr_t stack_page_top = (stack_top + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
  uintptr_t pool_base;
#if defined(__xtensa__)
  pool_base = xtensa_pool_base;
#else
  pool_base = PAGE_POOL_BASE;
#endif
  /* Populate page_linear[] and build the free stack using page indices.
   * page_linear[i] stores the 32-bit linear address for page i.
   * free_stack[] stores page_id_t indices, not void* pointers —
   * this allows pages beyond the 16-bit address range on i16. */
  free_top = 0;
  for (uint32_t i = 0u; i < page_count; i++) {
    uint32_t paddr = (uint32_t)pool_base + i * PAGE_SIZE;
    page_linear[i] = paddr;
    if (paddr < (uint32_t)stack_page_top) continue; /* overlaps kernel stack */
    free_stack[free_top++] = (page_id_t)i;
  }

  /* ── Boot-time memory map ─────────────────────────────────────────────── */
  klog("MM: memory map\n");
  klogf("MM:   kernel  %lx-%lx  %lu KB reserved\n",
        (unsigned long)SRAM_KERNEL_BASE,
        (unsigned long)(SRAM_KERNEL_BASE + SRAM_KERNEL_SIZE - 1u),
        (unsigned long)(SRAM_KERNEL_SIZE / 1024u));
  if (stack_top > bss_end)
    klogf("MM:     .data/.bss:  %lx B used, %lx B to stack top\n",
          (unsigned long)kern_used, (unsigned long)(stack_top - bss_end));
  else
    klogf("MM:     .data/.bss:  %lx B used\n", (unsigned long)kern_used);

  uint32_t actual_base =
      (free_top > 0) ? page_linear[free_stack[0]] : (uint32_t)pool_base;
  klogf("MM:   pages   %lx-%lx %lu KB (%u x 4 KB, all free)\n",
        (unsigned long)actual_base,
        (unsigned long)(pool_base + page_count * PAGE_SIZE - 1u),
        (unsigned long)(free_top * PAGE_SIZE / 1024u), free_top);
#if !defined(__m68k__) && !defined(__xtensa__) && !defined(__ia16__)
  klogf("MM:   io_buf  %lx-%lx  %lu KB\n", (unsigned long)SRAM_IOBUF_BASE,
        (unsigned long)(SRAM_IOBUF_BASE + SRAM_IOBUF_SIZE - 1u),
        (unsigned long)(SRAM_IOBUF_SIZE / 1024u));
  klogf("MM:   dma     %lx-%lx  %lu KB\n", (unsigned long)SRAM_DMA_BASE,
        (unsigned long)(SRAM_DMA_BASE + SRAM_DMA_SIZE - 1u),
        (unsigned long)(SRAM_DMA_SIZE / 1024u));
#endif

#ifdef PPAP_TESTS
  /* ── kmem self-test ───────────────────────────────────────────────────── */
  typedef struct {
    uint32_t a;
    uint32_t b;
  } test_obj_t;
  static uint8_t test_mem[4 * sizeof(test_obj_t)];
  static kmem_pool_t test_pool;
  kmem_pool_init(&test_pool, test_mem, sizeof(test_obj_t), 4u);

  test_obj_t *o1 = kmem_alloc(&test_pool); /* 3 free */
  test_obj_t *o2 = kmem_alloc(&test_pool); /* 2 free */
  kmem_free(&test_pool, o1);               /* 3 free */
  test_obj_t *o3 = kmem_alloc(&test_pool); /* 2 free */
  (void)o2;
  (void)o3;

  uint32_t ok = (o1 != NULL) && (o2 != NULL) && (o3 != NULL) &&
                (kmem_free_count(&test_pool) == 2u);

  klogf("MM: kmem self-test %s\n", ok ? "PASSED" : "FAILED");
#endif

#if defined(PPAP_TESTS) && (defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__))
  /* ── XIP verification and benchmark (ARM only) ───────────────────────── */
  xip_verify();
#endif
}

void *page_alloc(void) {
  uint32_t saved = spin_lock_irqsave(SPIN_PAGE);
  void *p = NULL;
  if (free_top != 0u) {
    /* Pick the highest-address free page.  Single-page allocations
     * (stacks, scratch) cluster at the top of the pool, leaving the
     * low end free for large contiguous allocations (ELF images).
     * O(free_top) scan — negligible for pools ≤ 256 pages. */
    uint32_t best = 0;
    for (uint32_t i = 1; i < free_top; i++) {
      if (free_stack[i] > free_stack[best])
        best = i;
    }
    page_id_t id = free_stack[best];
    free_stack[best] = free_stack[--free_top];
    p = (void *)(uintptr_t)page_linear[id];
  } else {
    oom_count++;
  }
  spin_unlock_irqrestore(SPIN_PAGE, saved);
  if (!p) klog("MM: OOM: page_alloc failed\n");
  return p;
}

void *page_alloc_at(void *addr) {
  uintptr_t target = (uintptr_t)addr;

  /* Validate: must be page-aligned and within the runtime pool */
  uintptr_t pb = page_pool_base();
  if (target < pb || target >= pb + page_count * PAGE_SIZE)
    return NULL;
  if (target & (PAGE_SIZE - 1u)) return NULL;

  page_id_t target_id = (page_id_t)((target - pb) / PAGE_SIZE);

  uint32_t saved = spin_lock_irqsave(SPIN_PAGE);
  void *result = NULL;

  /* Scan the free stack for the requested page */
  for (uint32_t i = 0u; i < free_top; i++) {
    if (free_stack[i] == target_id) {
      /* Remove by swapping with the top element */
      free_top--;
      free_stack[i] = free_stack[free_top];
      result = addr;
      break;
    }
  }

  spin_unlock_irqrestore(SPIN_PAGE, saved);
  return result;
}

/* ── Stack backtrace (RISC-V) ─────────────────────────────────────────────
 *
 * Walk the s0 (frame pointer) chain.  Each frame stores:
 *   [fp-4] = return address (ra)
 *   [fp-8] = previous frame pointer
 *
 * Requires -fno-omit-frame-pointer in CFLAGS.
 */
#if defined(__riscv)
static void stack_backtrace(void) {
  uintptr_t fp;
  __asm__ volatile("mv %0, s0" : "=r"(fp));

  klog("  backtrace:\n");
  for (uint32_t depth = 0; depth < 16 && fp; depth++) {
    uintptr_t ra = *(uintptr_t *)(fp - 4);
    uintptr_t prev_fp = *(uintptr_t *)(fp - 8);
    klogf("    #%u ra=%lx fp=%lx\n", depth, (unsigned long)ra, (unsigned long)fp);
    if (prev_fp <= fp) break; /* stack grows down — prev fp must be higher */
    fp = prev_fp;
  }
}
#else
static void stack_backtrace(void) {
  /* Not yet implemented for this architecture */
}
#endif

void page_free(void *page) {
  /* Rudimentary double-free / out-of-range guard */
  uintptr_t addr = (uintptr_t)page;
  uintptr_t pb = page_pool_base();
  if (addr < pb || addr >= pb + page_count * PAGE_SIZE)
    return; /* ignore bogus pointer rather than corrupt the stack */

  page_id_t id = (page_id_t)((addr - pb) / PAGE_SIZE);

  uint32_t saved = spin_lock_irqsave(SPIN_PAGE);

  /* Scan for double-free: O(free_top) ≈ O(51) at 133 MHz ≈ 1 µs */
  for (uint32_t i = 0u; i < free_top; i++) {
    if (free_stack[i] == id) {
      spin_unlock_irqrestore(SPIN_PAGE, saved);
      klogf("MM: double-free @ %lx (ra=%lx)\n", (unsigned long)addr,
            (unsigned long)(uintptr_t)__builtin_return_address(0));
      stack_backtrace();
      return;
    }
  }

  if (free_top < page_count) free_stack[free_top++] = id;
  spin_unlock_irqrestore(SPIN_PAGE, saved);
}

uintptr_t page_pool_base(void) {
#if defined(__xtensa__)
  return xtensa_pool_base;
#else
  return PAGE_POOL_BASE;
#endif
}

uint32_t page_free_count(void) {
  uint32_t saved = spin_lock_irqsave(SPIN_PAGE);
  uint32_t count = free_top;
  spin_unlock_irqrestore(SPIN_PAGE, saved);
  return count;
}

/* ── Bitmap helpers for contiguous allocation ────────────────────────────── */

/* Build a bitmap of free pages: bit i set = page i is free.
 * Must be called with SPIN_PAGE held. */
static void build_free_bitmap(uint32_t *bmap, uint32_t n_words) {
  for (uint32_t w = 0; w < n_words; w++) bmap[w] = 0;
  for (uint32_t i = 0; i < free_top; i++) {
    uint32_t idx = free_stack[i];
    if (idx < page_count) bmap[idx / 32] |= (1u << (idx % 32));
  }
}

static int bmap_test(const uint32_t *bmap, uint32_t idx) {
  return (bmap[idx / 32] >> (idx % 32)) & 1;
}

uint32_t page_max_contiguous(void) {
  uint32_t saved = spin_lock_irqsave(SPIN_PAGE);
  uint32_t n_words = (page_count + 31) / 32;
  uint32_t bmap[n_words];
  build_free_bitmap(bmap, n_words);

  uint32_t best = 0, run = 0;
  for (uint32_t i = 0; i < page_count; i++) {
    if (bmap_test(bmap, i)) {
      run++;
      if (run > best) best = run;
    } else {
      run = 0;
    }
  }

  spin_unlock_irqrestore(SPIN_PAGE, saved);
  return best;
}

/* Allocate n_pages contiguous pages.  Uses a bitmap scan to find a run,
 * then removes the pages from the free stack. O(page_count). */
uint8_t *page_alloc_contiguous(uint32_t n_pages) {
  if (n_pages == 0) return NULL;
  if (n_pages == 1) return (uint8_t *)page_alloc();

  uint32_t saved = spin_lock_irqsave(SPIN_PAGE);

  /* Build bitmap of free pages */
  uint32_t n_words = (page_count + 31) / 32;
  uint32_t bmap[n_words];
  build_free_bitmap(bmap, n_words);

  /* Scan for a contiguous run of n_pages free pages */
  uint32_t run_start = 0, run_len = 0;
  uint32_t found_start = 0;
  int found = 0;

  for (uint32_t i = 0; i < page_count; i++) {
    if (bmap_test(bmap, i)) {
      if (run_len == 0) run_start = i;
      run_len++;
      if (run_len >= n_pages) {
        found_start = run_start;
        found = 1;
        break;
      }
    } else {
      run_len = 0;
    }
  }

  if (!found) {
    spin_unlock_irqrestore(SPIN_PAGE, saved);
    return NULL;
  }

  /* Remove the found pages from the free stack by page index. */
  uint32_t new_top = 0;
  for (uint32_t i = 0; i < free_top; i++) {
    page_id_t pid = free_stack[i];
    if (pid >= found_start && pid < found_start + n_pages) {
      /* This page is being allocated — skip it */
    } else {
      free_stack[new_top++] = free_stack[i];
    }
  }
  free_top = new_top;

  uintptr_t base_addr = page_pool_base() + found_start * PAGE_SIZE;
  spin_unlock_irqrestore(SPIN_PAGE, saved);
  return (uint8_t *)(uintptr_t)base_addr;
}

/* Contiguous allocation returning base page_id_t (i16-safe). */
page_id_t mm_page_alloc_contiguous(uint32_t n_pages) {
  if (n_pages == 0) return PAGE_ID_INVALID;
  if (n_pages == 1) return mm_page_alloc();

  uint32_t saved = spin_lock_irqsave(SPIN_PAGE);

  uint32_t n_words = (page_count + 31) / 32;
  uint32_t bmap[n_words];
  build_free_bitmap(bmap, n_words);

  uint32_t run_start = 0, run_len = 0;
  uint32_t found_start = 0;
  int found = 0;

  for (uint32_t i = 0; i < page_count; i++) {
    if (bmap_test(bmap, i)) {
      if (run_len == 0) run_start = i;
      run_len++;
      if (run_len >= n_pages) {
        found_start = run_start;
        found = 1;
        break;
      }
    } else {
      run_len = 0;
    }
  }

  if (!found) {
    spin_unlock_irqrestore(SPIN_PAGE, saved);
    return PAGE_ID_INVALID;
  }

  uint32_t new_top = 0;
  for (uint32_t i = 0; i < free_top; i++) {
    page_id_t pid = free_stack[i];
    if (pid >= found_start && pid < found_start + n_pages) {
    } else {
      free_stack[new_top++] = free_stack[i];
    }
  }
  free_top = new_top;

  spin_unlock_irqrestore(SPIN_PAGE, saved);
  return (page_id_t)found_start;
}

/* ── Page-indexed API ────────────────────────────────────────────────────── */

page_id_t mm_page_alloc(void) {
  uint32_t saved = spin_lock_irqsave(SPIN_PAGE);
  if (free_top == 0u) {
    oom_count++;
    spin_unlock_irqrestore(SPIN_PAGE, saved);
    klog("MM: OOM: page_alloc failed\n");
    return PAGE_ID_INVALID;
  }
  /* Pick highest-index free page (same policy as page_alloc) */
  uint32_t best = 0;
  for (uint32_t i = 1; i < free_top; i++) {
    if (free_stack[i] > free_stack[best])
      best = i;
  }
  page_id_t id = free_stack[best];
  free_stack[best] = free_stack[--free_top];
  spin_unlock_irqrestore(SPIN_PAGE, saved);
  return id;
}

uint32_t mm_page_linear(page_id_t id) {
  if (id >= page_count) return 0;
  return page_linear[id];
}

void mm_page_free(page_id_t id) {
  if (id == PAGE_ID_INVALID || id >= page_count) return;
  uint32_t saved = spin_lock_irqsave(SPIN_PAGE);
  /* Double-free check */
  for (uint32_t i = 0; i < free_top; i++) {
    if (free_stack[i] == id) {
      spin_unlock_irqrestore(SPIN_PAGE, saved);
      klogf("MM: double-free page %u\n", (unsigned)id);
      return;
    }
  }
  if (free_top < page_count) free_stack[free_top++] = id;
  spin_unlock_irqrestore(SPIN_PAGE, saved);
}

void mm_page_read(page_id_t id, uint16_t off, void *buf, uint16_t len) {
  if (id == PAGE_ID_INVALID || id >= page_count || len == 0) return;
#if defined(__ia16__)
  /* Compute segment:offset from 20-bit linear address.
   * rep movsb copies DS:SI → ES:DI.  We want src=far page, dst=SS:buf. */
  uint32_t linear = page_linear[id] + off;
  uint16_t seg = (uint16_t)(linear >> 4);
  uint16_t ofs = (uint16_t)(linear & 0x000Fu);
  uint16_t dst = (uint16_t)(uintptr_t)buf;
  __asm__ volatile (
    "push %%ds\n\t"
    "push %%es\n\t"
    "mov  %%ss, %%ax\n\t"
    "mov  %%ax, %%es\n\t"  /* ES = SS (destination) */
    "mov  %0, %%ds\n\t"    /* DS = source segment */
    "cld\n\t"
    "rep movsb\n\t"
    "pop  %%es\n\t"
    "pop  %%ds"
    :
    : "r"(seg), "S"(ofs), "D"(dst), "c"(len)
    : "ax", "memory", "cc"
  );
#else
  __builtin_memcpy(buf,
                   (const uint8_t *)(uintptr_t)page_linear[id] + off,
                   len);
#endif
}

void mm_page_write(page_id_t id, uint16_t off, const void *buf, uint16_t len) {
  if (id == PAGE_ID_INVALID || id >= page_count || len == 0) return;
#if defined(__ia16__)
  /* rep movsb copies DS:SI → ES:DI.  We want src=SS:buf, dst=far page. */
  uint32_t linear = page_linear[id] + off;
  uint16_t seg = (uint16_t)(linear >> 4);
  uint16_t ofs = (uint16_t)(linear & 0x000Fu);
  uint16_t src = (uint16_t)(uintptr_t)buf;
  __asm__ volatile (
    "push %%es\n\t"
    "mov  %0, %%es\n\t"    /* ES = destination segment */
    "cld\n\t"
    "rep movsb\n\t"
    "pop  %%es"
    :
    : "r"(seg), "S"(src), "D"(ofs), "c"(len)
    : "memory", "cc"
  );
#else
  __builtin_memcpy((uint8_t *)(uintptr_t)page_linear[id] + off,
                   buf, len);
#endif
}

#if !defined(__ia16__)
void *mm_page_to_ptr(page_id_t id) {
  if (id == PAGE_ID_INVALID || id >= page_count) return NULL;
  return (void *)(uintptr_t)page_linear[id];
}
#endif

page_id_t mm_ptr_to_page(void *ptr) {
  uintptr_t addr = (uintptr_t)ptr;
  uintptr_t pb = page_pool_base();
  if (addr < pb) return PAGE_ID_INVALID;
  page_id_t id = (page_id_t)((addr - pb) / PAGE_SIZE);
  if (id >= page_count) return PAGE_ID_INVALID;
  return id;
}
