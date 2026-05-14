/*
 * sys_mem.c — Memory management syscall implementations
 *
 *   sys_brk(addr) — adjust the program break (heap boundary)
 *
 * The heap starts at brk_base (end of .data+.bss) and grows upward
 * within tracked page-backed user memory slots. The initial
 * image pages are recorded by the loader; later heap expansion pages are
 * appended on demand via mem_region_alloc_at() to ensure contiguity.
 */

#include <string.h>

#include "common/errno.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/mm/page.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/syscall/syscall.h"

#if defined(__ia16__)
#define I16_USER_SEG_PAGES 16u
#define I16_USER_STACK_PAGE (I16_USER_SEG_PAGES - 1u)

static page_id_t i16_user_segment_base(const pcb_t *p) {
  if (!p) return PAGE_ID_INVALID;
  if (p->image.data.base_page != PAGE_ID_INVALID)
    return p->image.data.base_page;
  return proc_page_backed_base(p);
}

static uint16_t i16_user_page_count(uintptr_t end_off) {
  if (end_off == 0u) return 0u;
  return (uint16_t)((end_off + PAGE_SIZE - 1u) / PAGE_SIZE);
}

static void i16_user_zero_page(page_id_t page_id) {
  mem_region_page_zero(page_id, 0, PAGE_SIZE);
}

static void sys_brk_zero_owned_range(page_id_t base_id, uintptr_t start_off,
                                     uintptr_t end_off) {
  uintptr_t pos = start_off;

  if (end_off <= start_off) return;

  while (pos < end_off) {
    uint16_t pg_off = (uint16_t)(pos % PAGE_SIZE);
    uint16_t chunk = PAGE_SIZE - pg_off;
    if (chunk > end_off - pos) chunk = (uint16_t)(end_off - pos);
    mem_region_page_zero(base_id + (page_id_t)(pos / PAGE_SIZE), pg_off, chunk);
    pos += chunk;
  }
}

static int i16_mmap_range_is_free(const pcb_t *p, uint16_t start_page,
                                  uint16_t num_pages, uint16_t brk_pages) {
  uint16_t end_page = (uint16_t)(start_page + num_pages);

  if (!p) return 0;
  if (end_page > I16_USER_STACK_PAGE) return 0;
  if (start_page < brk_pages) return 0;
  for (uint16_t page = start_page; page < end_page; page++) {
    if (p->user_pages[page] != PAGE_ID_INVALID) return 0;
  }
  return 1;
}
#endif

/* ── sys_brk ─────────────────────────────────────────────────────────────────
 */

long sys_brk(long addr) {
  /* Query: addr == 0 → return current break.
   *
   * Linux semantics: brk ALWAYS returns the current program break.
   * On failure, it returns the unchanged break (never a negative errno).
   * User-space libc relies on this to detect brk failure and fall back
   * to mmap. */
  if (addr == 0) return (long)(current->brk_current);

  uintptr_t new_brk = (uintptr_t)addr;

  /* Cannot shrink below initial break */
  if (new_brk < current->brk_base)
    return (long)(current->brk_current); /* unchanged = failure */

#if defined(__ia16__)
  page_id_t base_id = i16_user_segment_base(current);
  uint16_t old_pages;
  uint16_t new_pages;
  uintptr_t old_top = current->brk_current;

  if (base_id == PAGE_ID_INVALID)
    return (long)(current->brk_current); /* unchanged = failure */

  if (new_brk > (uintptr_t)I16_USER_STACK_PAGE * PAGE_SIZE)
    return (long)(current->brk_current); /* unchanged = failure */

  old_pages = i16_user_page_count(old_top);
  new_pages = i16_user_page_count(new_brk);

  if (new_pages > old_pages) {
    for (uint16_t page = old_pages; page < new_pages; page++) {
      if (current->user_pages[page] != PAGE_ID_INVALID)
        return (long)(current->brk_current); /* collides with mmap/stack */
    }
    for (uint16_t page = old_pages; page < new_pages; page++)
      current->user_pages[page] = base_id + (page_id_t)page;
  } else if (new_pages < old_pages) {
    for (uint16_t page = new_pages; page < old_pages; page++)
      current->user_pages[page] = PAGE_ID_INVALID;
  }

  if (new_brk > old_top) sys_brk_zero_owned_range(base_id, old_top, new_brk);

  current->brk_current = new_brk;
  return (long)new_brk;
#else
  /* Calculate old and new page counts from the tracked page-backed base.
   * The loader records the initial page-backed user image here, and
   * sys_brk appends heap pages contiguously after it. */
  page_id_t page0_id = proc_page_backed_base(current);
  if (page0_id == PAGE_ID_INVALID)
    return (long)(current->brk_current); /* unchanged = failure */
  uintptr_t page0_base = (uintptr_t)mem_region_page_linear(page0_id);
  uintptr_t old_top = current->brk_current;
  uintptr_t new_top = new_brk;
  uint32_t old_pages;
  uint32_t new_pages;

  old_pages = (old_top - page0_base + PAGE_SIZE - 1) / PAGE_SIZE;
  new_pages = (new_top - page0_base + PAGE_SIZE - 1) / PAGE_SIZE;

  if (new_pages > USER_PAGES_MAX)
    return (long)(current->brk_current); /* unchanged = failure */

  /* Expand: allocate contiguous pages after existing ones */
  for (uint32_t i = old_pages; i < new_pages; i++) {
    proc_image_segment_t page_region = {0};
    uintptr_t target = page0_base + i * PAGE_SIZE;
    if (mem_region_alloc_at(&page_region, PPAP_MEM_RAM_DATA,
                            (void *)(uintptr_t)target, PAGE_SIZE,
                            PROC_IMAGE_SEG_WRITABLE) < 0) {
      /* Roll back any pages we just allocated */
      proc_release_tracked_pages(current, old_pages, i);
      return (long)(current->brk_current); /* unchanged = failure */
    }
    memset(page_region.base, 0, PAGE_SIZE);
    proc_track_page(current, i, mem_region_ptr_to_page(page_region.base));
  }

  /* Shrink: free excess pages */
  proc_release_tracked_pages(current, new_pages, old_pages);

  current->brk_current = new_brk;
  return (long)(new_brk);
#endif
}

/* ── sys_mmap2 ────────────────────────────────────────────────────────────────
 */
/*
 * Anonymous-only mmap.  Allocates page-backed RAM data via mem_region and
 * tracks pages in user_pages[] (high slots, searched top-down).
 * File-backed mmap is not supported.
 *
 * mmap2 takes page-offset (not byte-offset): pgoff = byte_offset / 4096.
 */
#define MAP_ANONYMOUS 0x20u
#define MAP_PRIVATE 0x02u
#define MAP_FIXED 0x10u

/* Find num_pages contiguous free slots in user_pages[], searching from
 * the top down (high slots) to avoid colliding with brk growth (low
 * slots).  Returns the start slot index, or -1 if no run found. */
static int mmap_find_slots(const pcb_t *p, uint32_t num_pages) {
  uint32_t run = 0;
  for (uint32_t i = USER_PAGES_MAX; i > 0; i--) {
    if (p->user_pages[i - 1] == PAGE_ID_INVALID) {
      run++;
      if (run >= num_pages) return (int)(i - 1);
    } else {
      run = 0;
    }
  }
  return -1;
}

long sys_mmap2(uintptr_t addr, size_t len, uint32_t prot, uint32_t flags,
               uint32_t fd, uint32_t pgoff) {
  (void)prot;
  (void)pgoff;

  /* Only anonymous+private mappings supported */
  if (!(flags & MAP_ANONYMOUS)) return -(long)ENOSYS;

  /* fd must be -1 for anonymous */
  if ((int)fd != -1) return -(long)EINVAL;

  if (len == 0) return -(long)EINVAL;

  uint32_t num_pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;

#if defined(__ia16__)
  page_id_t base_id = i16_user_segment_base(current);
  uint16_t brk_pages;

  if (base_id == PAGE_ID_INVALID) return -(long)ENOMEM;
  if (num_pages == 0 || num_pages > I16_USER_STACK_PAGE) return -(long)ENOMEM;

  brk_pages = i16_user_page_count(current->brk_current);

  if ((flags & MAP_FIXED) && addr != 0) {
    uint16_t start_page;
    uint16_t end_page;

    if ((addr & (PAGE_SIZE - 1u)) != 0) return -(long)EINVAL;
    start_page = (uint16_t)(addr / PAGE_SIZE);
    end_page = (uint16_t)(start_page + num_pages);
    if (end_page > I16_USER_STACK_PAGE) return -(long)ENOMEM;
    if (!i16_mmap_range_is_free(current, start_page, (uint16_t)num_pages,
                                brk_pages))
      return -(long)ENOMEM;
    for (uint16_t page = start_page; page < end_page; page++) {
      i16_user_zero_page(base_id + (page_id_t)page);
      current->user_pages[page] = base_id + (page_id_t)page;
    }
    return (long)((uintptr_t)start_page * PAGE_SIZE);
  }

  for (uint16_t start_page =
           (uint16_t)(I16_USER_STACK_PAGE - (uint16_t)num_pages);
       start_page >= brk_pages; start_page--) {
    if (!i16_mmap_range_is_free(current, start_page, (uint16_t)num_pages,
                                brk_pages)) {
      if (start_page == 0u) break;
      continue;
    }
    for (uint16_t page = start_page;
         page < (uint16_t)(start_page + (uint16_t)num_pages); page++) {
      i16_user_zero_page(base_id + (page_id_t)page);
      current->user_pages[page] = base_id + (page_id_t)page;
    }
    return (long)((uintptr_t)start_page * PAGE_SIZE);
  }

  return -(long)ENOMEM;
#else

  /* Find contiguous free slots in user_pages[] (top-down) */
  int slot = mmap_find_slots(current, num_pages);
  if (slot < 0) return -(long)ENOMEM;

  /* MAP_FIXED: try to allocate at specific address */
  if ((flags & MAP_FIXED) && addr != 0) {
    proc_image_segment_t region = {0};
    void *base = (void *)addr;
    if (mem_region_alloc_at(&region, PPAP_MEM_RAM_DATA, base,
                            num_pages * PAGE_SIZE,
                            PROC_IMAGE_SEG_WRITABLE) < 0) {
      return -(long)ENOMEM;
    }
    memset(region.base, 0, region.size);
    page_id_t base_id = mem_region_ptr_to_page(region.base);
    for (uint32_t i = 0; i < num_pages; i++)
      current->user_pages[(uint32_t)slot + i] = base_id + (page_id_t)i;
    return (long)((uintptr_t)base);
  }

  {
    proc_image_segment_t region = {0};
    if (mem_region_alloc(&region, PPAP_MEM_RAM_DATA, num_pages * PAGE_SIZE,
                         PROC_IMAGE_SEG_WRITABLE) < 0) {
      return -(long)ENOMEM;
    }
    memset(region.base, 0, region.size);
    page_id_t base_id = mem_region_ptr_to_page(region.base);
    for (uint32_t i = 0; i < num_pages; i++)
      current->user_pages[(uint32_t)slot + i] = base_id + (page_id_t)i;
    return (long)((uintptr_t)region.base);
  }
#endif
}

/* ── sys_munmap ───────────────────────────────────────────────────────────────
 */

long sys_munmap(uintptr_t addr, size_t len) {
  if (addr == 0 || len == 0) return -(long)EINVAL;

  uint32_t num_pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;

#if defined(__ia16__)
  uint16_t start_page;
  uint16_t end_page;
  uint16_t brk_pages = i16_user_page_count(current->brk_current);

  if ((addr & (PAGE_SIZE - 1u)) != 0) return -(long)EINVAL;

  start_page = (uint16_t)(addr / PAGE_SIZE);
  end_page = (uint16_t)(start_page + num_pages);
  if (end_page > I16_USER_STACK_PAGE) return -(long)EINVAL;
  if (start_page < brk_pages) return -(long)EINVAL;

  for (uint16_t page = start_page; page < end_page; page++) {
    if (current->user_pages[page] == PAGE_ID_INVALID) return -(long)EINVAL;
  }
  for (uint16_t page = start_page; page < end_page; page++)
    current->user_pages[page] = PAGE_ID_INVALID;
  return 0;
#else

  /* Find the matching page in user_pages[] by linear address */
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) {
    if (current->user_pages[i] == PAGE_ID_INVALID) continue;
    if (mem_region_page_linear(current->user_pages[i]) != (uint32_t)addr)
      continue;

    /* Free num_pages contiguous slots from this position */
    for (uint32_t j = 0; j < num_pages && (i + j) < USER_PAGES_MAX; j++) {
      if (current->user_pages[i + j] == PAGE_ID_INVALID) break;
      mem_region_free_tracked_page_id(current->user_pages[i + j]);
      current->user_pages[i + j] = PAGE_ID_INVALID;
    }
    return 0;
  }

  /* Not found in user_pages — try freeing as a single page anyway.
   * User-space may mmap then munmap pages we didn't track (edge case). */
  proc_image_segment_t region =
      proc_image_segment_make((void *)(uintptr_t)addr, num_pages * PAGE_SIZE,
                              PPAP_MEM_RAM_DATA, PROC_IMAGE_SEG_WRITABLE);
  mem_region_free(&region);
  return 0;
#endif
}
