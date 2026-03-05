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
#include "kernel/signal/signal.h"
#include "kernel/errno.h"
#include <string.h>

/* Maximum PT_LOAD segments we handle */
#define MAX_LOAD_SEGS  4

int do_execve(pcb_t *p, const char *path, const char *const *argv)
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
    uint32_t file_size = vn->size;
    int nseg = elf_load_segments(ehdr, file_base, segs, MAX_LOAD_SEGS,
                                 file_size);
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
        if (data_pages > USER_PAGES_MAX) {
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
        if (elf_find_got(ehdr, file_base, &got_info, file_size) == 0) {
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

        /* ── 7b. R_ARM_RELATIVE relocations (.rel.dyn from PIE builds) ─── */
        /* PIE binaries have function-pointer arrays in .data (e.g.
         * applet_main[]) whose entries are raw link-time vaddrs.
         * .rel.dyn lists these locations so we can fix them up. */
        elf_rel_info_t rel_info;
        if (elf_find_rel(ehdr, file_base, &rel_info, file_size) == 0) {
            const elf32_rel_t *rel =
                (const elf32_rel_t *)(file_base + rel_info.offset);
            uint32_t n_rel = rel_info.size / sizeof(elf32_rel_t);

            for (uint32_t i = 0; i < n_rel; i++) {
                if (ELF32_R_TYPE(rel[i].r_info) != R_ARM_RELATIVE)
                    continue;

                uint32_t off = rel[i].r_offset;

                /* Only patch words in the data segment (SRAM).
                 * Text-segment relocations can't be patched (flash XIP). */
                if (off < data_seg->p_vaddr ||
                    off >= data_seg->p_vaddr + data_seg->p_memsz)
                    continue;

                /* Skip words already handled by GOT relocation */
                if (got_sram_addr != 0 &&
                    off >= got_info.addr &&
                    off < got_info.addr + got_info.size)
                    continue;

                uint32_t off_in_sram = off - data_seg->p_vaddr;
                uint32_t *word = (uint32_t *)(sram_page + off_in_sram);
                uint32_t val = *word;

                if (val == 0)
                    continue;

                if (val < data_seg->p_vaddr) {
                    /* Text/rodata reference → flash */
                    *word = val + flash_text_base;
                } else {
                    /* Data/BSS reference → SRAM */
                    *word = val - data_seg->p_vaddr +
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
        for (int i = 0; i < USER_PAGES_MAX; i++) {
            if (p->user_pages[i]) {
                page_free(p->user_pages[i]);
                p->user_pages[i] = NULL;
            }
        }
        vnode_put(vn);
        return -(int)ENOMEM;
    }
    p->stack_page = stack;

    /* ── 8a. Build argc/argv/envp/auxv at top of stack ─────────────────
     *
     * musl's _start reads:  argc = [SP], argv = SP+4, ...
     *
     * Layout (high → low):
     *   argv string data (each NUL-terminated)
     *   <8-byte alignment padding>
     *   auxv[1] = {AT_NULL, 0}
     *   auxv[0] = {AT_PAGESZ, PAGE_SIZE}
     *   envp[0] = NULL
     *   argv[argc] = NULL
     *   argv[argc-1..0] = pointers to string data
     *   argc
     *              ← user_sp (PSP after exception return)
     */
    {
        uint32_t stack_top = (uint32_t)(uintptr_t)stack + PAGE_SIZE;
        uint32_t sp = stack_top;

        /* Count arguments */
        int argc = 0;
        if (argv) {
            while (argv[argc] != NULL && argc < 32)
                argc++;
        }
        if (argc == 0) {
            /* No argv provided (kernel init): use path as argv[0] */
            argc = 1;
            argv = NULL;   /* signal fallback below */
        }

        /* Copy argument strings to top of stack (high → low) */
        uint32_t str_addrs[32];
        if (argv) {
            for (int i = argc - 1; i >= 0; i--) {
                uint32_t len = (uint32_t)strlen(argv[i]) + 1;
                sp -= len;
                memcpy((void *)(uintptr_t)sp, argv[i], len);
                str_addrs[i] = sp;
            }
        } else {
            /* Fallback: use path as sole argument */
            uint32_t len = (uint32_t)strlen(path) + 1;
            sp -= len;
            memcpy((void *)(uintptr_t)sp, path, len);
            str_addrs[0] = sp;
        }

        /* Align down to 8 bytes before pushing word-sized data.
         * Total words below: 4 (auxv) + 1 (envp NULL) + (argc+1) (argv
         * array + NULL) + 1 (argc) = argc + 7.  If this is odd, add a
         * padding word to keep the final SP 8-byte aligned (ARM AAPCS). */
        sp &= ~7u;
        if ((argc + 7) & 1)
            sp -= 4;   /* padding word for 8-byte alignment */

        /* auxv[1]: AT_NULL */
        sp -= 4; *(uint32_t *)(uintptr_t)sp = 0;          /* val */
        sp -= 4; *(uint32_t *)(uintptr_t)sp = 0;          /* AT_NULL */
        /* auxv[0]: AT_PAGESZ */
        sp -= 4; *(uint32_t *)(uintptr_t)sp = PAGE_SIZE;  /* val */
        sp -= 4; *(uint32_t *)(uintptr_t)sp = 6;          /* AT_PAGESZ */

        /* envp[0] = NULL */
        sp -= 4; *(uint32_t *)(uintptr_t)sp = 0;

        /* argv[argc] = NULL */
        sp -= 4; *(uint32_t *)(uintptr_t)sp = 0;
        /* argv[argc-1..0] */
        for (int i = argc - 1; i >= 0; i--) {
            sp -= 4; *(uint32_t *)(uintptr_t)sp = str_addrs[i];
        }

        /* argc */
        sp -= 4; *(uint32_t *)(uintptr_t)sp = (uint32_t)argc;

        /* ── 9. Set up the exception frame ─────────────────────────────── */
        proc_setup_stack(p, (void (*)(void))(uintptr_t)entry, sp);
    }

    /* Patch r9 (GOT base) in the software frame.
     * proc_setup_stack builds: [r4, r5, r6, r7, r8, r9, r10, r11]
     * at p->sp, so r9 is at offset 5 (index 5). */
    if (got_sram_addr) {
        uint32_t *sw = (uint32_t *)(uintptr_t)p->sp;
        sw[5] = got_sram_addr;
    }

    /* Save GOT base in PCB for vfork — child needs parent's r9 */
    p->got_base = got_sram_addr;

    /* ── 10. Set process comm from executable basename ────────────────── */
    {
        const char *base = path;
        for (const char *s = path; *s; s++) {
            if (*s == '/')
                base = s + 1;
        }
        size_t clen = strlen(base);
        if (clen > 15) clen = 15;
        memcpy(p->comm, base, clen);
        p->comm[clen] = '\0';
    }

    /* ── 11. Set working directory ─────────────────────────────────────── */
    if (current)
        memcpy(p->cwd, current->cwd, sizeof(p->cwd));
    else
        strcpy(p->cwd, "/");

    /* ── 13. Reset signal state (POSIX exec semantics) ────────────────── */
    /*  Caught signals → SIG_DFL; SIG_IGN signals stay ignored.
     *  Clear pending and blocked masks so the new image starts clean. */
    for (int i = 0; i < NSIG; i++) {
        if (p->sig_handlers[i] != SIG_IGN)
            p->sig_handlers[i] = SIG_DFL;
    }
    p->sig_pending = 0;
    p->sig_blocked = 0;

    /* ── 14. Release the vnode ─────────────────────────────────────────── */
    vnode_put(vn);

    return 0;
}
