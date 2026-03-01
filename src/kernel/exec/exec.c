/*
 * exec.c — ELF binary loader for PPAP
 *
 * Loads an ELF binary from romfs flash into SRAM and prepares a process
 * to execute it.  Code/data segments are copied to their linked SRAM
 * addresses (USER_CODE_BASE region).  The stack is allocated from the
 * page pool.  The process is ready for scheduling after do_execve()
 * returns successfully.
 */

#include "exec.h"
#include "elf.h"
#include "kernel/vfs/vfs.h"
#include "kernel/mm/page.h"
#include "kernel/fd/fd.h"
#include "kernel/errno.h"
#include "config.h"
#include <string.h>

/* Maximum PT_LOAD segments we handle */
#define MAX_LOAD_SEGS  4

int do_execve(pcb_t *p, const char *path)
{
    vnode_t *vn = NULL;
    int err;

    /* ── 1. Look up the binary in the VFS ──────────────────────────────── */
    err = vfs_lookup(path, &vn);
    if (err < 0)
        return err;

    if (vn->type != VNODE_FILE || vn->xip_addr == NULL) {
        vnode_put(vn);
        return -(int)ENOEXEC;
    }

    /* ── 2. Validate the ELF header ────────────────────────────────────── */
    const uint8_t *file_base = (const uint8_t *)vn->xip_addr;
    const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)file_base;

    err = elf_validate(ehdr);
    if (err < 0) {
        vnode_put(vn);
        return err;
    }

    /* ── 3. Extract PT_LOAD segments ───────────────────────────────────── */
    elf32_phdr_t segs[MAX_LOAD_SEGS];
    int nseg = elf_load_segments(ehdr, file_base, segs, MAX_LOAD_SEGS);
    if (nseg <= 0) {
        vnode_put(vn);
        return -(int)ENOEXEC;
    }

    /* ── 4. Load each segment into SRAM ────────────────────────────────── */
    int pages_used = 0;
    void *allocated_pages[MAX_LOAD_SEGS];

    for (int i = 0; i < nseg && i < MAX_LOAD_SEGS; i++) {
        const elf32_phdr_t *seg = &segs[i];

        /* Determine which page(s) this segment occupies.
         * For Phase 3, each segment fits in one page (hello.elf is 68 bytes). */
        void *page_addr = (void *)(uintptr_t)(seg->p_vaddr & ~(PAGE_SIZE - 1u));
        void *page = page_alloc_at(page_addr);
        if (!page) {
            /* Clean up previously allocated pages */
            for (int j = 0; j < pages_used; j++)
                page_free(allocated_pages[j]);
            vnode_put(vn);
            return -(int)ENOMEM;
        }
        allocated_pages[pages_used++] = page;

        /* Copy file data from flash to SRAM */
        uint8_t *dest = (uint8_t *)(uintptr_t)seg->p_vaddr;
        const uint8_t *src = file_base + seg->p_offset;
        if (seg->p_filesz > 0)
            memcpy(dest, src, seg->p_filesz);

        /* Zero BSS (p_memsz > p_filesz) */
        if (seg->p_memsz > seg->p_filesz)
            memset(dest + seg->p_filesz, 0, seg->p_memsz - seg->p_filesz);
    }

    /* Store code pages in user_pages[] */
    for (int i = 0; i < pages_used && i < 4; i++)
        p->user_pages[i] = allocated_pages[i];

    /* ── 5. Allocate stack page ────────────────────────────────────────── */
    void *stack = page_alloc();
    if (!stack) {
        for (int j = 0; j < pages_used; j++)
            page_free(allocated_pages[j]);
        vnode_put(vn);
        return -(int)ENOMEM;
    }
    p->stack_page = stack;

    /* ── 6. Set up the exception frame ─────────────────────────────────── */
    uint32_t entry = elf_entry(ehdr);
    proc_setup_stack(p, (void (*)(void))(uintptr_t)entry);

    /* ── 7. Initialise file descriptors (stdin/stdout/stderr → tty) ────── */
    fd_stdio_init(p);

    /* ── 8. Set working directory ──────────────────────────────────────── */
    extern pcb_t *current;
    if (current)
        memcpy(p->cwd, current->cwd, sizeof(p->cwd));
    else
        strcpy(p->cwd, "/");

    /* ── 9. Release the vnode ──────────────────────────────────────────── */
    vnode_put(vn);

    return 0;
}
