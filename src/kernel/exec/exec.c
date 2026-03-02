/*
 * exec.c — ELF binary loader for PPAP (XIP + PIC/GOT relocation)
 *
 * Loads a PIC ELF binary for Execute-In-Place (XIP) from flash:
 *   - .text + .rodata stay in flash (no SRAM copy)
 *   - .got + .data + .bss are copied to contiguous SRAM page(s)
 *   - GOT entries are relocated to point to actual flash/SRAM addresses
 *   - r9 is loaded with the SRAM address of the .got section
 *
 * The binary is linked at address 0 with two PT_LOAD segments:
 *   text (PF_R|PF_X): code + rodata  → executes from flash
 *   data (PF_R|PF_W): .got + .data + .bss → lives in SRAM
 */

#include "exec.h"
#include "elf.h"
#include "kernel/vfs/vfs.h"
#include "kernel/mm/page.h"
#include "kernel/fd/fd.h"
#include "kernel/errno.h"
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

    /* ── 4. Identify text and data segments ────────────────────────────── */
    const elf32_phdr_t *text_seg = NULL;
    const elf32_phdr_t *data_seg = NULL;

    for (int i = 0; i < nseg && i < MAX_LOAD_SEGS; i++) {
        if (segs[i].p_flags & PF_X)
            text_seg = &segs[i];
        else if (segs[i].p_flags & PF_W)
            data_seg = &segs[i];
    }

    if (!text_seg) {
        vnode_put(vn);
        return -(int)ENOEXEC;
    }

    /* ── 5. Text segment: stays in flash (XIP) ─────────────────────────── */
    uint32_t flash_text_base =
        (uint32_t)(uintptr_t)file_base + text_seg->p_offset;

    /* Compute the runtime entry point in flash */
    uint32_t e_entry = elf_entry(ehdr);
    uint32_t entry = flash_text_base + (e_entry & ~1u) - text_seg->p_vaddr;
    entry |= (e_entry & 1u);   /* restore Thumb bit */

    /* ── 6. Data segment: copy to contiguous SRAM page(s) ────────────── */
    uint8_t *sram_page = NULL;
    uint32_t got_sram_addr = 0;

    if (data_seg) {
        /* Calculate how many contiguous pages we need */
        uint32_t data_pages =
            (data_seg->p_memsz + PAGE_SIZE - 1) / PAGE_SIZE;
        if (data_pages == 0)
            data_pages = 1;
        if (data_pages > 4) {
            vnode_put(vn);
            return -(int)ENOMEM;
        }

        /* Allocate contiguous pages for the data segment.
         * Single-page: fast path via page_alloc().
         * Multi-page:  scan the pool for a contiguous block using
         *              page_alloc_at() to claim each page. */
        if (data_pages == 1) {
            sram_page = (uint8_t *)page_alloc();
        } else {
            for (uint32_t base = PAGE_POOL_BASE;
                 base + data_pages * PAGE_SIZE <=
                     PAGE_POOL_BASE + PAGE_POOL_SIZE;
                 base += PAGE_SIZE) {
                uint32_t k;
                for (k = 0; k < data_pages; k++) {
                    void *pg = page_alloc_at(
                        (void *)(uintptr_t)(base + k * PAGE_SIZE));
                    if (!pg) {
                        for (uint32_t l = 0; l < k; l++)
                            page_free(
                                (void *)(uintptr_t)(base + l * PAGE_SIZE));
                        break;
                    }
                }
                if (k == data_pages) {
                    sram_page = (uint8_t *)(uintptr_t)base;
                    break;
                }
            }
        }
        if (!sram_page) {
            vnode_put(vn);
            return -(int)ENOMEM;
        }

        /* Copy initialised data (.got + .data) from flash */
        if (data_seg->p_filesz > 0)
            memcpy(sram_page, file_base + data_seg->p_offset,
                   data_seg->p_filesz);

        /* Zero BSS (p_memsz > p_filesz) */
        if (data_seg->p_memsz > data_seg->p_filesz)
            memset(sram_page + data_seg->p_filesz, 0,
                   data_seg->p_memsz - data_seg->p_filesz);

        /* ── 7. GOT relocation ─────────────────────────────────────────── */
        elf_got_info_t got_info;
        if (elf_find_got(ehdr, file_base, &got_info) == 0) {
            uint32_t got_offset_in_data =
                got_info.addr - data_seg->p_vaddr;
            got_sram_addr =
                (uint32_t)(uintptr_t)sram_page + got_offset_in_data;

            uint32_t *got =
                (uint32_t *)(sram_page + got_offset_in_data);
            uint32_t n_entries = got_info.size / 4;

            for (uint32_t i = 0; i < n_entries; i++) {
                uint32_t val = got[i];
                if (val == 0)
                    continue;   /* reserved / unused entry */

                if (val < data_seg->p_vaddr) {
                    /* Text/rodata reference → points into flash */
                    got[i] = val + flash_text_base;
                } else {
                    /* Data/BSS reference → points into SRAM page(s) */
                    got[i] = val - data_seg->p_vaddr +
                             (uint32_t)(uintptr_t)sram_page;
                }
            }
        }

        /* Store all data pages in user_pages[] */
        for (uint32_t i = 0; i < data_pages; i++)
            p->user_pages[i] = sram_page + i * PAGE_SIZE;

        /* Set initial program break at end of .data+.bss */
        uint32_t data_end = (uint32_t)(uintptr_t)sram_page + data_seg->p_memsz;
        p->brk_base    = data_end;
        p->brk_current = data_end;
    }

    /* ── 8. Allocate stack page ────────────────────────────────────────── */
    void *stack = page_alloc();
    if (!stack) {
        for (int i = 0; i < 4; i++) {
            if (p->user_pages[i]) {
                page_free(p->user_pages[i]);
                p->user_pages[i] = NULL;
            }
        }
        vnode_put(vn);
        return -(int)ENOMEM;
    }
    p->stack_page = stack;

    /* ── 9. Set up the exception frame ─────────────────────────────────── */
    proc_setup_stack(p, (void (*)(void))(uintptr_t)entry);

    /* Patch r9 (GOT base) in the software frame.
     * proc_setup_stack builds: [r4, r5, r6, r7, r8, r9, r10, r11]
     * at p->sp, so r9 is at offset 5 (index 5). */
    if (got_sram_addr) {
        uint32_t *sw = (uint32_t *)(uintptr_t)p->sp;
        sw[5] = got_sram_addr;
    }

    /* Save GOT base in PCB for vfork — child needs parent's r9 */
    p->got_base = got_sram_addr;

    /* ── 10. Initialise file descriptors (stdin/stdout/stderr → tty) ──── */
    fd_stdio_init(p);

    /* ── 11. Set working directory ─────────────────────────────────────── */
    extern pcb_t *current;
    if (current)
        memcpy(p->cwd, current->cwd, sizeof(p->cwd));
    else
        strcpy(p->cwd, "/");

    /* ── 12. Release the vnode ─────────────────────────────────────────── */
    vnode_put(vn);

    return 0;
}
