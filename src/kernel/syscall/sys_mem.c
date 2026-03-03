/*
 * sys_mem.c — Memory management syscall implementations
 *
 *   sys_brk(addr) — adjust the program break (heap boundary)
 *
 * The heap starts at brk_base (end of .data+.bss) and grows upward
 * within user_pages[0..7].  user_pages[0..N-1] are the data/GOT pages
 * allocated by do_execve; remaining slots are heap expansion pages
 * allocated on demand via page_alloc_at() to ensure contiguity.
 */

#include "syscall.h"
#include "../proc/proc.h"
#include "../mm/page.h"
#include "../errno.h"
#include <string.h>

/* ── sys_brk ───────────────────────────────────────────────────────────────── */

long sys_brk(long addr)
{
    /* Query: addr == 0 → return current break.
     *
     * Linux semantics: brk ALWAYS returns the current program break.
     * On failure, it returns the unchanged break (never a negative errno).
     * musl relies on this to detect brk failure and fall back to mmap. */
    if (addr == 0)
        return (long)current->brk_current;

    uint32_t new_brk = (uint32_t)addr;

    /* Cannot shrink below initial break */
    if (new_brk < current->brk_base)
        return (long)current->brk_current;   /* unchanged = failure */

    /* Calculate old and new page counts from user_pages[0] base.
     * user_pages[0..N-1] hold the data segment (allocated by do_execve).
     * Remaining slots are heap pages, allocated contiguously via
     * page_alloc_at() so brk addresses map to valid physical memory. */
    uint32_t page0_base = (uint32_t)(uintptr_t)current->user_pages[0];
    uint32_t old_top = current->brk_current;
    uint32_t new_top = new_brk;

    uint32_t old_pages = (old_top - page0_base + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t new_pages = (new_top - page0_base + PAGE_SIZE - 1) / PAGE_SIZE;

    if (new_pages > 8)
        return (long)current->brk_current;   /* unchanged = failure */

    /* Expand: allocate contiguous pages after existing ones */
    for (uint32_t i = old_pages; i < new_pages; i++) {
        uint32_t target = page0_base + i * PAGE_SIZE;
        void *pg = page_alloc_at((void *)(uintptr_t)target);
        if (!pg) {
            /* Roll back any pages we just allocated */
            for (uint32_t j = old_pages; j < i; j++) {
                page_free(current->user_pages[j]);
                current->user_pages[j] = NULL;
            }
            return (long)current->brk_current;   /* unchanged = failure */
        }
        memset(pg, 0, PAGE_SIZE);
        current->user_pages[i] = pg;
    }

    /* Shrink: free excess pages */
    for (uint32_t i = new_pages; i < old_pages; i++) {
        if (current->user_pages[i]) {
            page_free(current->user_pages[i]);
            current->user_pages[i] = NULL;
        }
    }

    current->brk_current = new_brk;
    return (long)new_brk;
}

/* ── sys_mmap2 ──────────────────────────────────────────────────────────────── */
/*
 * Anonymous-only mmap.  Allocates pages from the page pool and tracks
 * them in current->mmap_regions[].  File-backed mmap is not supported.
 *
 * mmap2 takes page-offset (not byte-offset): pgoff = byte_offset / 4096.
 */
#define MAP_ANONYMOUS  0x20u
#define MAP_PRIVATE    0x02u
#define MAP_FIXED      0x10u

long sys_mmap2(uint32_t addr, uint32_t len, uint32_t prot,
               uint32_t flags, uint32_t fd, uint32_t pgoff)
{
    (void)prot; (void)pgoff;

    /* Only anonymous+private mappings supported */
    if (!(flags & MAP_ANONYMOUS))
        return -(long)ENOSYS;

    /* fd must be -1 for anonymous */
    if ((int)fd != -1)
        return -(long)EINVAL;

    if (len == 0)
        return -(long)EINVAL;

    uint32_t num_pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Find a free mmap_regions slot */
    int slot = -1;
    for (int i = 0; i < MMAP_REGIONS_MAX; i++) {
        if (current->mmap_regions[i].addr == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -(long)ENOMEM;

    /* MAP_FIXED: try to allocate at specific address */
    if ((flags & MAP_FIXED) && addr != 0) {
        void *base = (void *)(uintptr_t)addr;
        for (uint32_t i = 0; i < num_pages; i++) {
            void *pg = page_alloc_at((void *)((uintptr_t)base + i * PAGE_SIZE));
            if (!pg) {
                /* Rollback */
                for (uint32_t j = 0; j < i; j++)
                    page_free((void *)((uintptr_t)base + j * PAGE_SIZE));
                return -(long)ENOMEM;
            }
            memset(pg, 0, PAGE_SIZE);
        }
        current->mmap_regions[slot].addr  = base;
        current->mmap_regions[slot].pages = num_pages;
        return (long)(uintptr_t)base;
    }

    /* Non-fixed: allocate from pool. Try contiguous first, fall back to
     * single-page if only 1 page needed. */
    if (num_pages == 1) {
        void *pg = page_alloc();
        if (!pg)
            return -(long)ENOMEM;
        memset(pg, 0, PAGE_SIZE);
        current->mmap_regions[slot].addr  = pg;
        current->mmap_regions[slot].pages = 1;
        return (long)(uintptr_t)pg;
    }

    /* Multi-page: scan for contiguous block */
    uint32_t pool_base = PAGE_POOL_BASE;
    uint32_t pool_end  = pool_base + PAGE_POOL_SIZE;

    for (uint32_t base = pool_base; base + num_pages * PAGE_SIZE <= pool_end;
         base += PAGE_SIZE) {
        uint32_t k;
        for (k = 0; k < num_pages; k++) {
            void *pg = page_alloc_at((void *)(uintptr_t)(base + k * PAGE_SIZE));
            if (!pg) {
                for (uint32_t l = 0; l < k; l++)
                    page_free((void *)(uintptr_t)(base + l * PAGE_SIZE));
                break;
            }
        }
        if (k == num_pages) {
            void *result = (void *)(uintptr_t)base;
            for (uint32_t i = 0; i < num_pages; i++)
                memset((void *)(uintptr_t)(base + i * PAGE_SIZE), 0, PAGE_SIZE);
            current->mmap_regions[slot].addr  = result;
            current->mmap_regions[slot].pages = num_pages;
            return (long)(uintptr_t)result;
        }
    }

    return -(long)ENOMEM;
}

/* ── sys_munmap ─────────────────────────────────────────────────────────────── */

long sys_munmap(uint32_t addr, uint32_t len)
{
    if (addr == 0 || len == 0)
        return -(long)EINVAL;

    /* Find the matching mmap region */
    for (int i = 0; i < MMAP_REGIONS_MAX; i++) {
        if (current->mmap_regions[i].addr == (void *)(uintptr_t)addr) {
            uint32_t pages = current->mmap_regions[i].pages;
            for (uint32_t j = 0; j < pages; j++) {
                page_free((void *)(uintptr_t)(addr + j * PAGE_SIZE));
            }
            current->mmap_regions[i].addr  = NULL;
            current->mmap_regions[i].pages = 0;
            return 0;
        }
    }

    /* Not found in mmap_regions — try freeing as a single page anyway.
     * musl may mmap then munmap pages we didn't track (edge case). */
    uint32_t num_pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t j = 0; j < num_pages; j++)
        page_free((void *)(uintptr_t)(addr + j * PAGE_SIZE));
    return 0;
}
