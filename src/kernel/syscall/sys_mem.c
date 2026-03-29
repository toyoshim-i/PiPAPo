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

#include "../common/errno.h"
#include "../mm/mem_region.h"
#include "../proc/proc.h"
#include "syscall.h"

/* ── sys_brk ─────────────────────────────────────────────────────────────────
 */

long sys_brk(long addr) {
  /* Query: addr == 0 → return current break.
   *
   * Linux semantics: brk ALWAYS returns the current program break.
   * On failure, it returns the unchanged break (never a negative errno).
   * musl relies on this to detect brk failure and fall back to mmap. */
  if (addr == 0) return (long)(current->brk_current);

  uintptr_t new_brk = (uintptr_t)addr;

  /* Cannot shrink below initial break */
  if (new_brk < current->brk_base)
    return (long)(current->brk_current); /* unchanged = failure */

  /* Calculate old and new page counts from the tracked page-backed base.
   * The loader records the initial page-backed user image here, and
   * sys_brk appends heap pages contiguously after it. */
  uintptr_t page0_base = (uintptr_t)proc_page_backed_base(current);
  uintptr_t old_top = current->brk_current;
  uintptr_t new_top = new_brk;
  uint32_t old_pages;
  uint32_t new_pages;

  if (!page0_base)
    return (long)(current->brk_current); /* unchanged = failure */

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
    proc_track_page(current, i, page_region.base);
  }

  /* Shrink: free excess pages */
  proc_release_tracked_pages(current, new_pages, old_pages);

  current->brk_current = new_brk;
  return (long)(new_brk);
}

/* ── sys_mmap2 ────────────────────────────────────────────────────────────────
 */
/*
 * Anonymous-only mmap.  Allocates page-backed RAM data via mem_region and
 * tracks the resulting contiguous region in current->mmap_regions[].
 * File-backed mmap is not supported.
 *
 * mmap2 takes page-offset (not byte-offset): pgoff = byte_offset / 4096.
 */
#define MAP_ANONYMOUS 0x20u
#define MAP_PRIVATE 0x02u
#define MAP_FIXED 0x10u

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

  /* Find a free mmap_regions slot */
  int slot = -1;
  for (int i = 0; i < MMAP_REGIONS_MAX; i++) {
    if (current->mmap_regions[i].addr == NULL) {
      slot = i;
      break;
    }
  }
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
    current->mmap_regions[slot].addr = base;
    current->mmap_regions[slot].pages = num_pages;
    return (long)((uintptr_t)base);
  }

  {
    proc_image_segment_t region = {0};
    if (mem_region_alloc(&region, PPAP_MEM_RAM_DATA, num_pages * PAGE_SIZE,
                         PROC_IMAGE_SEG_WRITABLE) < 0) {
      return -(long)ENOMEM;
    }
    memset(region.base, 0, region.size);
    current->mmap_regions[slot].addr = region.base;
    current->mmap_regions[slot].pages = num_pages;
    return (long)((uintptr_t)region.base);
  }
}

/* ── sys_munmap ───────────────────────────────────────────────────────────────
 */

long sys_munmap(uintptr_t addr, size_t len) {
  if (addr == 0 || len == 0) return -(long)EINVAL;

  /* Find the matching mmap region */
  for (int i = 0; i < MMAP_REGIONS_MAX; i++) {
    if (current->mmap_regions[i].addr == (void *)(uintptr_t)addr) {
      uint32_t pages = current->mmap_regions[i].pages;
      proc_image_segment_t region = proc_image_segment_make(
          (void *)(uintptr_t)addr, pages * PAGE_SIZE, PPAP_MEM_RAM_DATA,
          PROC_IMAGE_SEG_WRITABLE);
      mem_region_free(&region);
      current->mmap_regions[i].addr = NULL;
      current->mmap_regions[i].pages = 0;
      return 0;
    }
  }

  /* Not found in mmap_regions — try freeing as a single page anyway.
   * musl may mmap then munmap pages we didn't track (edge case). */
  uint32_t num_pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
  proc_image_segment_t region = proc_image_segment_make(
      (void *)(uintptr_t)addr, num_pages * PAGE_SIZE, PPAP_MEM_RAM_DATA,
      PROC_IMAGE_SEG_WRITABLE);
  mem_region_free(&region);
  return 0;
}
