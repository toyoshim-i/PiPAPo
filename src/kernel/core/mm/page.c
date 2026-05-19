/*
 * page.c — Physical page allocator
 *
 * Free-stack implementation: a fixed array of PAGE_COUNT_MAX page_id_t values,
 * managed as a LIFO stack.  Alloc pops the top; free pushes onto the top.
 * Both operations are O(1) with no heap or dynamic storage needed.
 *
 * page_id_t is a linear page number (address / PAGE_SIZE), not a page-pool
 * index.  That lets non-ia16 code form page references for arbitrary linear
 * buffers directly, while the allocator still tracks which page IDs belong to
 * the pool.
 *
 * mm_init() also prints the boot-time memory map, showing:
 *   - actual kernel data usage (measured from __bss_end at link time)
 *   - page pool, I/O buffer, and DMA/Core1 region sizes
 * This lets you track how much kernel data grows as Phase 1+ code is added.
 */

#include "kernel/core/mm/page.h"

#include <stddef.h>

#include "kernel/common/mod/mod_vfs.h"
#include "kernel/common/spinlock.h"
#include "kernel/core/backtrace.h"
#include "kernel/core/mm/kmem.h"
#include "kernel/core/mm/mem_helper.h"
#include "kernel/core/mm/page_pool.h"
#include "kernel/core/mm/region.h"
#include "kernel/core/panic.h"

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

/* ── Allocator state ────────────────────────────────────────────────────────
 *
 * The actual free-list lives in page_pool.c; this file wraps it with
 * SPIN_PAGE, OOM/double-free klogf messages, and the memory-map print.
 * The split also lets the pure allocator be unit-tested on the host
 * (tests/host/test_page_pool.c). */
uint32_t page_count = 0u; /* runtime pool page count (set by mm_init)  */
uint32_t oom_count = 0u;  /* number of page_alloc() failures */

/* Page alloc/free tracing — opt-in via `PPAP_PAGE_TRACE=1` at build time.
 * Prints one line per allocator op with the resulting pool state, useful
 * for diffing allocator policy changes against a known-good run.  Not
 * wired to PPAP_TESTS because the trace itself runs inside SPIN_PAGE, so
 * leaving it always-on for the test lane has turned out to interact
 * poorly with early-boot ordering on some targets.
 *
 * Called with SPIN_PAGE held so the free/max-contig snapshot matches the
 * op just done. */
static void page_trace_tail(const char *op, unsigned id, unsigned arg) {
#if defined(PPAP_PAGE_TRACE) && PPAP_PAGE_TRACE
  unsigned free_total = (unsigned)pool_free_total();
  unsigned max_contig = (unsigned)pool_max_contig();
  mod_vfs.klogf("PAGE: %s id=%u arg=%u free=%u maxc=%u\n", op, id, arg,
                free_total, max_contig);
#else
  (void)op;
  (void)id;
  (void)arg;
#endif
}

/* ── Internal helpers ───────────────────────────────────────────────────────
 */

static inline uint32_t page_id_linear(page_id_t id) {
  return (uint32_t)id * PAGE_SIZE;
}

static inline page_id_t linear_page_id(uint32_t addr) {
  return (page_id_t)(addr / PAGE_SIZE);
}

static inline page_id_t pool_base_page_id(void) {
  return linear_page_id(page_pool_base());
}

static int pool_page_id_to_index(page_id_t id, uint32_t *index) {
  page_id_t base = pool_base_page_id();
  uint32_t rel;

  if (id < base) return 0;
  rel = (uint32_t)(id - base);
  if (rel >= page_count) return 0;
  if (index) *index = rel;
  return 1;
}

/* ── Public API ─────────────────────────────────────────────────────────────
 */

/* Aligned base of the runtime page pool.  Set during mm_init: for
 * targets with a fixed/linker-located pool this stays at PAGE_POOL_BASE;
 * targets that allocate the pool at boot overwrite it through
 * mem_helper_init_pool.  Must be uint32_t — on ia16 the page pool can
 * sit above 64 KB (linear address space > 16 bits) and uintptr_t there
 * is the 16-bit near-pointer type, which would silently truncate the
 * upper half of the base address. */
static uint32_t s_pool_base;

void mm_init(void) {
  uintptr_t bss_end = (uintptr_t)&__bss_end;
  uintptr_t stack_top = (uintptr_t)&__stack_top;
  uintptr_t kern_used = bss_end - SRAM_KERNEL_BASE;

  /* Detect available RAM and set runtime page_count.  Default values
   * cover targets with a fixed/linker-located pool; arch and target
   * overlays of mem_helper_init_pool overwrite both page_count and
   * s_pool_base when a runtime probe or heap grab is required. */
  page_count = PAGE_COUNT_MAX;
  s_pool_base = PAGE_POOL_BASE;
  if (mem_helper_init_pool(&s_pool_base) < 0) panic("page pool init failed\n");

  /* Build the free stack: push pages from the pool that don't overlap
   * with the kernel stack.  On QEMU (flat memory model) the initial
   * stack may extend into the page pool region.  Skip those pages. */
  uintptr_t stack_page_top = (stack_top + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
  uint32_t pool_base = (uint32_t)s_pool_base;
  /* Hand the runtime page inventory to the pure allocator core.  Walk
   * pages in address order; when we hit a skipped-page boundary, flush
   * the run-in-progress as one add_range() call.  The allocator expects
   * ranges to be sorted ascending, which this loop trivially satisfies. */
  pool_reset();
  {
    page_id_t run_base = 0;
    uint16_t run_len = 0;
    uint32_t run_first = 0;
    for (uint32_t i = 0u; i < page_count; i++) {
      uint32_t paddr = (uint32_t)pool_base + i * PAGE_SIZE;
      int skipped = (paddr < (uint32_t)stack_page_top);
      if (!skipped) {
        if (run_len == 0) {
          run_base = linear_page_id(paddr);
          run_first = i;
        }
        run_len++;
      } else if (run_len > 0) {
        pool_add(run_base, run_len);
        run_len = 0;
      }
      (void)run_first;
    }
    if (run_len > 0) pool_add(run_base, run_len);
  }

  /* ── Boot-time memory map ─────────────────────────────────────────────── */
  mod_vfs.klogf("MM: memory map\n");
  mod_vfs.klogf("MM:   kernel  %lx-%lx  %lu KB reserved\n",
                (unsigned long)SRAM_KERNEL_BASE,
                (unsigned long)(SRAM_KERNEL_BASE + SRAM_KERNEL_SIZE - 1u),
                (unsigned long)(SRAM_KERNEL_SIZE / 1024u));
  if (stack_top > bss_end)
    mod_vfs.klogf("MM:     .data/.bss:  %lx B used, %lx B to stack top\n",
                  (unsigned long)kern_used,
                  (unsigned long)(stack_top - bss_end));
  else
    mod_vfs.klogf("MM:     .data/.bss:  %lx B used\n",
                  (unsigned long)kern_used);

  uint32_t free_total = pool_free_total();
  mod_vfs.klogf("MM:   pages   %lx-%lx %lu KB (%u x 4 KB, all free)\n",
                (unsigned long)pool_base,
                (unsigned long)(pool_base + page_count * PAGE_SIZE - 1u),
                (unsigned long)(free_total * PAGE_SIZE / 1024u),
                (unsigned)free_total);
#if !defined(__m68k__) && !defined(__xtensa__) && !defined(__ia16__)
  mod_vfs.klogf("MM:   io_buf  %lx-%lx  %lu KB\n",
                (unsigned long)SRAM_IOBUF_BASE,
                (unsigned long)(SRAM_IOBUF_BASE + SRAM_IOBUF_SIZE - 1u),
                (unsigned long)(SRAM_IOBUF_SIZE / 1024u));
  mod_vfs.klogf("MM:   dma     %lx-%lx  %lu KB\n", (unsigned long)SRAM_DMA_BASE,
                (unsigned long)(SRAM_DMA_BASE + SRAM_DMA_SIZE - 1u),
                (unsigned long)(SRAM_DMA_SIZE / 1024u));
#endif

  mem_helper_post_init();
}

/* ── Allocation ─────────────────────────────────────────────────────────── */

page_id_t page_alloc(void) {
  uint32_t saved = spin_lock_irqsave(SPIN_PAGE);
  page_id_t id = pool_take_n(1);
  if (id == PAGE_ID_INVALID) oom_count++;
  page_trace_tail("alloc1", (unsigned)id, 1);
  spin_unlock_irqrestore(SPIN_PAGE, saved);
  if (id == PAGE_ID_INVALID) mod_vfs.klogf("MM: OOM: page_alloc failed\n");
  return id;
}

page_id_t page_alloc_n(uint32_t n_pages) {
  if (n_pages == 0 || n_pages > 0xFFFFu) return PAGE_ID_INVALID;
  uint32_t saved = spin_lock_irqsave(SPIN_PAGE);
  page_id_t id = pool_take_n((uint16_t)n_pages);
  page_trace_tail("alloc_contig", (unsigned)id, (unsigned)n_pages);
  spin_unlock_irqrestore(SPIN_PAGE, saved);
  return id;
}

page_id_t page_alloc_largest(uint32_t min_pages, uint32_t max_pages,
                             uint32_t *got_pages) {
  if (got_pages) *got_pages = 0;
  if (min_pages == 0 || max_pages < min_pages || min_pages > 0xFFFFu)
    return PAGE_ID_INVALID;
  if (max_pages > 0xFFFFu) max_pages = 0xFFFFu;

  uint32_t saved = spin_lock_irqsave(SPIN_PAGE);
  uint16_t got = 0;
  page_id_t base =
      pool_take_largest((uint16_t)min_pages, (uint16_t)max_pages, &got);
  page_trace_tail("alloc_largest", (unsigned)base, (unsigned)got);
  spin_unlock_irqrestore(SPIN_PAGE, saved);
  if (got_pages) *got_pages = got;
  return base;
}

int page_alloc_at(page_id_t id) {
  if (id == PAGE_ID_INVALID) return -1;
  uint32_t saved = spin_lock_irqsave(SPIN_PAGE);
  int rc = pool_take_at(id);
  page_trace_tail(rc == 0 ? "alloc_at" : "alloc_at_FAIL", (unsigned)id, 1);
  spin_unlock_irqrestore(SPIN_PAGE, saved);
  return rc;
}

void page_free(page_id_t id) {
  if (id == PAGE_ID_INVALID || !pool_page_id_to_index(id, NULL)) return;
  uint32_t saved = spin_lock_irqsave(SPIN_PAGE);
  if (pool_is_free(id)) {
    spin_unlock_irqrestore(SPIN_PAGE, saved);
    mod_vfs.klogf("MM: double-free page %u (ra=%lx)\n", (unsigned)id,
                  (unsigned long)(uintptr_t)__builtin_return_address(0));
    stack_backtrace();
    return;
  }
  int rc = pool_release(id, 1);
  page_trace_tail("free1", (unsigned)id, 1);
  spin_unlock_irqrestore(SPIN_PAGE, saved);
  if (rc < 0) {
    mod_vfs.klogf("MM: PANIC: free-list full, page %u leaked\n", (unsigned)id);
  }
}

/* ── Pool introspection ─────────────────────────────────────────────────── */

uint32_t page_pool_base(void) { return (uint32_t)s_pool_base; }

uint32_t page_free_count(void) {
  uint32_t saved = spin_lock_irqsave(SPIN_PAGE);
  uint32_t count = pool_free_total();
  spin_unlock_irqrestore(SPIN_PAGE, saved);
  return count;
}

uint32_t page_max_contiguous(void) {
  uint32_t saved = spin_lock_irqsave(SPIN_PAGE);
  uint32_t best = pool_max_contig();
  spin_unlock_irqrestore(SPIN_PAGE, saved);
  return best;
}

/* ── Page-payload access ────────────────────────────────────────────────── */

uint32_t page_linear(page_id_t id) {
  if (id == PAGE_ID_INVALID) return 0;
  return (uint32_t)page_id_linear(id);
}

void page_read(page_id_t id, uint16_t off, void *buf, uint16_t len) {
  if (id == PAGE_ID_INVALID || len == 0) return;
#if defined(__ia16__)
  /* Compute segment:offset from 20-bit linear address.
   * rep movsb copies DS:SI → ES:DI.  We want src=far page, dst=SS:buf. */
  uint32_t linear = (uint32_t)page_id_linear(id) + off;
  uint16_t seg = (uint16_t)(linear >> 4);
  uint16_t ofs = (uint16_t)(linear & 0x000Fu);
  uint16_t dst = (uint16_t)(uintptr_t)buf;
  __asm__ volatile(
      "push %%ds\n\t"
      "push %%es\n\t"
      "mov  %%ss, %%ax\n\t"
      "mov  %%ax, %%es\n\t" /* ES = SS (destination) */
      "mov  %0, %%ds\n\t"   /* DS = source segment */
      "cld\n\t"
      "rep movsb\n\t"
      "pop  %%es\n\t"
      "pop  %%ds"
      :
      : "r"(seg), "S"(ofs), "D"(dst), "c"(len)
      : "ax", "memory", "cc");
#else
  __builtin_memcpy(buf, (const uint8_t *)page_id_linear(id) + off, len);
#endif
}

void page_write(page_id_t id, uint16_t off, const void *buf, uint16_t len) {
  if (id == PAGE_ID_INVALID || len == 0) return;
#if defined(__ia16__)
  /* rep movsb copies DS:SI → ES:DI.  We want src=SS:buf, dst=far page. */
  uint32_t linear = (uint32_t)page_id_linear(id) + off;
  uint16_t seg = (uint16_t)(linear >> 4);
  uint16_t ofs = (uint16_t)(linear & 0x000Fu);
  uint16_t src = (uint16_t)(uintptr_t)buf;
  __asm__ volatile(
      "push %%ds\n\t"
      "push %%es\n\t"
      "mov  %%ss, %%ax\n\t"
      "mov  %%ax, %%ds\n\t" /* DS = SS (source: kernel data) */
      "mov  %0, %%es\n\t"   /* ES = destination segment */
      "cld\n\t"
      "rep movsb\n\t"
      "pop  %%es\n\t"
      "pop  %%ds"
      :
      : "r"(seg), "S"(src), "D"(ofs), "c"(len)
      : "ax", "memory", "cc");
#else
  __builtin_memcpy((uint8_t *)page_id_linear(id) + off, buf, len);
#endif
}

void page_zero(page_id_t id, uint16_t off, uint16_t len) {
  if (id == PAGE_ID_INVALID || len == 0) return;
#if defined(__ia16__)
  /* rep stosb fills ES:DI with AL.  DI and CX are declared as
   * input+output so GCC re-reads them from memory after the asm — the
   * instruction advances DI by `len` and decrements CX to zero, which
   * would desync any C-level variable GCC had mirrored to those
   * registers. */
  uint32_t linear = (uint32_t)page_id_linear(id) + off;
  uint16_t seg = (uint16_t)(linear >> 4);
  uint16_t ofs = (uint16_t)(linear & 0x000Fu);
  uint16_t cx = len;
  __asm__ volatile(
      "push %%es\n\t"
      "mov  %2, %%es\n\t"
      "xor  %%al, %%al\n\t"
      "cld\n\t"
      "rep stosb\n\t"
      "pop  %%es"
      : "+D"(ofs), "+c"(cx)
      : "r"(seg)
      : "ax", "memory", "cc");
#else
  __builtin_memset((uint8_t *)page_id_linear(id) + off, 0, len);
#endif
}

#if !defined(__ia16__)
void *page_to_ptr(page_id_t id) {
  if (id == PAGE_ID_INVALID) return NULL;
  return (void *)page_id_linear(id);
}
#endif

page_id_t page_from_ptr(void *ptr) {
  uintptr_t addr = (uintptr_t)ptr;

  if (addr == 0u) return PAGE_ID_INVALID;
  return linear_page_id(addr);
}
