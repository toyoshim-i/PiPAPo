/*
 * sys_mem.c — Memory management syscall implementations
 *
 *   sys_brk(addr) — adjust the program break (heap boundary)
 *
 * The heap starts at brk_base (end of .data+.bss) and grows upward
 * within user_pages[0..3].  user_pages[0] is the data/GOT page
 * allocated by do_execve; pages 1-3 are allocated on demand when
 * brk crosses a page boundary.
 */

#include "syscall.h"
#include "../proc/proc.h"
#include "../mm/page.h"
#include "../errno.h"
#include <string.h>

/* ── sys_brk ───────────────────────────────────────────────────────────────── */

long sys_brk(long addr)
{
    /* Query: addr == 0 → return current break */
    if (addr == 0)
        return (long)current->brk_current;

    uint32_t new_brk = (uint32_t)addr;

    /* Cannot shrink below initial break */
    if (new_brk < current->brk_base)
        return -(long)ENOMEM;

    /* Calculate old and new page counts from user_pages[0] base.
     * user_pages[0] is always the data page (allocated by do_execve).
     * user_pages[1..3] are heap expansion pages. */
    uint32_t page0_base = (uint32_t)(uintptr_t)current->user_pages[0];
    uint32_t old_top = current->brk_current;
    uint32_t new_top = new_brk;

    uint32_t old_pages = (old_top - page0_base + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t new_pages = (new_top - page0_base + PAGE_SIZE - 1) / PAGE_SIZE;

    if (new_pages > 4)
        return -(long)ENOMEM;   /* max 4 user_pages slots */

    /* Expand: allocate new pages */
    for (uint32_t i = old_pages; i < new_pages; i++) {
        void *pg = page_alloc();
        if (!pg) {
            /* Roll back any pages we just allocated */
            for (uint32_t j = old_pages; j < i; j++) {
                page_free(current->user_pages[j]);
                current->user_pages[j] = NULL;
            }
            return -(long)ENOMEM;
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
