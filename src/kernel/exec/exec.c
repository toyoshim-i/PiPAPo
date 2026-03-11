/*
 * exec.c — ELF binary loader for PPAP
 *
 * ARM: Execute-In-Place (XIP)
 *   - .text + .rodata stay in flash (execute in place)
 *   - .got + .data + .bss are copied to contiguous SRAM page(s)
 *   - GOT entries are relocated to point to actual text/SRAM addresses
 *   - GOT base register: r9 (set by kernel, compiler doesn't recalculate)
 *
 * m68k: Execute-In-Place (XIP) from romfs
 *   - .text stays in romfs (execute in place — romfs is in RAM)
 *   - .got + .data + .bss are copied to contiguous SRAM page(s)
 *   - With -msep-data, a5 holds the GOT base (set by kernel), so text
 *     and data locations are fully independent (no PC-relative GOT lookup)
 *   - Text relocations are rejected at load time (binaries must be
 *     compiled with -msep-data -fPIC)
 */

#include "exec.h"
#include "elf.h"
#include "exec_x68k.h"
#include "exec_cpm.h"
#include "exec_m68k_emu.h"
#include "kernel/vfs/vfs.h"
#include "kernel/mm/page.h"
#include "kernel/signal/signal.h"
#include "kernel/errno.h"
#include <string.h>

/* Maximum PT_LOAD segments we handle */
#define MAX_LOAD_SEGS  4

/* ── Contiguous page allocation helper ─────────────────────────────────── */

uint8_t *alloc_contiguous(uint32_t n_pages)
{
    return page_alloc_contiguous(n_pages);
}

/* ── Relocation helper: apply .rel.dyn / .rela.dyn ─────────────────────── */

static void apply_relocations(const elf32_ehdr_t *ehdr,
                              const uint8_t *file_base,
                              uint32_t file_size,
                              const elf32_phdr_t *text_seg,
                              const elf32_phdr_t *data_seg,
                              uint8_t *text_ram,
                              uint8_t *sram_page,
                              uint32_t text_base,
                              uint32_t got_sram_addr,
                              const elf_got_info_t *got_info)
{
    elf_rel_info_t rel_info;
    if (elf_find_rel(ehdr, file_base, &rel_info, file_size) != 0)
        return;

    const uint8_t *rel_base = file_base + rel_info.offset;
    uint32_t entry_size = rel_info.has_addend
        ? sizeof(elf32_rela_t) : sizeof(elf32_rel_t);
    uint32_t n_rel = rel_info.size / entry_size;

    /* Find .dynsym for JMP_SLOT resolution */
    elf_dynsym_info_t dynsym_info = {0, 0};
    const elf32_sym_t *dynsym = NULL;
    if (elf_find_dynsym(ehdr, file_base, &dynsym_info, file_size) == 0)
        dynsym = (const elf32_sym_t *)(file_base + dynsym_info.offset);

    for (uint32_t i = 0; i < n_rel; i++) {
        uint32_t r_offset, r_info;
        int32_t  r_addend = 0;

        if (rel_info.has_addend) {
            const elf32_rela_t *rela =
                (const elf32_rela_t *)(rel_base + i * entry_size);
            r_offset = rela->r_offset;
            r_info   = rela->r_info;
            r_addend = rela->r_addend;
        } else {
            const elf32_rel_t *rel =
                (const elf32_rel_t *)(rel_base + i * entry_size);
            r_offset = rel->r_offset;
            r_info   = rel->r_info;
        }

        uint32_t rtype = ELF32_R_TYPE(r_info);

        /* ── Handle JMP_SLOT: resolve PLT GOT entry ─────────── */
#if defined(__m68k__)
        if (rtype == R_68K_JMP_SLOT) {
#else
        if (rtype == R_ARM_JMP_SLOT) {
#endif
            if (!dynsym)
                continue;
            uint32_t sym_idx = ELF32_R_SYM(r_info);
            if (sym_idx >= dynsym_info.count)
                continue;
            uint32_t off = r_offset;
            if (off < data_seg->p_vaddr ||
                off >= data_seg->p_vaddr + data_seg->p_memsz)
                continue;
            uint32_t off_in_sram = off - data_seg->p_vaddr;
            uint32_t *word = (uint32_t *)(sram_page + off_in_sram);
            uint32_t sym_val = dynsym[sym_idx].st_value
                              + (uint32_t)r_addend;
            if (sym_val < data_seg->p_vaddr)
                *word = sym_val + text_base;
            else
                *word = sym_val - data_seg->p_vaddr +
                        (uint32_t)(uintptr_t)sram_page;
            continue;
        }

        /* ── Handle RELATIVE ────────────────────────────────── */
#if defined(__m68k__)
        if (rtype != R_68K_RELATIVE)
#else
        if (rtype != R_ARM_RELATIVE)
#endif
            continue;

        uint32_t off = r_offset;

        (void)text_seg;
        (void)text_ram;

        /* Only patch words in the data segment (SRAM) */
        if (off < data_seg->p_vaddr ||
            off >= data_seg->p_vaddr + data_seg->p_memsz)
            continue;

        /* Skip words already handled by GOT relocation */
        if (got_sram_addr != 0 && got_info &&
            off >= got_info->addr &&
            off < got_info->addr + got_info->size)
            continue;

        uint32_t off_in_sram = off - data_seg->p_vaddr;
        uint32_t *word = (uint32_t *)(sram_page + off_in_sram);

        uint32_t val = rel_info.has_addend
            ? (uint32_t)r_addend : *word;

        if (val == 0)
            continue;

        if (val < data_seg->p_vaddr)
            *word = val + text_base;
        else
            *word = val - data_seg->p_vaddr +
                    (uint32_t)(uintptr_t)sram_page;
    }
}

/* ── do_execve ─────────────────────────────────────────────────────────── */

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

    /* ── 2. Detect binary format ─────────────────────────────────────── */
    const uint8_t *file_base = (const uint8_t *)vn->xip_addr;
    uint32_t file_size = vn->size;

    /* Try Human68k X-format ("HU" magic) before ELF */
    if (x68k_detect(file_base, file_size)) {
        vnode_put(vn);
        return exec_x68k(p, file_base, file_size, path, argv);
    }

    /* Try CP/M .COM (detected by extension) */
    if (cpm_detect(path, file_base, file_size)) {
        vnode_put(vn);
        return exec_cpm(p, file_base, file_size, path, argv);
    }

    /* ── 2b. Validate the ELF header ───────────────────────────────────── */
    const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)file_base;

    err = elf_validate(ehdr);
    if (err < 0) {
        /* ELF validation failed — might be a cross-arch m68k binary or
         * a non-ELF format.  Try m68k emulation first (detects EM_68K
         * big-endian ELF that elf_validate rejects on ARM hosts). */
        if (m68k_emu_detect(ehdr)) {
            vnode_put(vn);
            return exec_m68k_emu(p, file_base, file_size, ehdr, path, argv);
        }
        /* Not ELF — try Human68k R-format (.r extension, raw binary) */
        if (r68k_detect(path, file_base, file_size)) {
            vnode_put(vn);
            return exec_r68k(p, file_base, file_size, path, argv);
        }
        vnode_put(vn);
        return err;
    }

    /* ── 3. Extract PT_LOAD segments ───────────────────────────────────── */
    elf32_phdr_t segs[MAX_LOAD_SEGS];
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

    uint32_t e_entry = elf_entry(ehdr);
    uint32_t entry;
    uint32_t text_base;    /* runtime base of text segment */
    uint8_t *sram_page = NULL;
    uint32_t got_sram_addr = 0;
    elf_got_info_t got_info = {0, 0, 0};

    /* ── Pre-allocate stack page before data pages ─────────────────────
     *
     * page_alloc() returns the most-recently-freed page (LIFO).  After a
     * process exits, its brk pages are near the top of the free stack.
     * If we allocated data pages first (via alloc_contiguous scanning
     * from the bottom), page_alloc() for the stack would pop a recently
     * freed brk page — often the page right after the data block.  That
     * blocks brk expansion → OOM.
     *
     * Pre-allocating the stack drains that dangerous LIFO entry before
     * alloc_contiguous runs.
     */
    void *stack = page_alloc();
    if (!stack) {
        vnode_put(vn);
        return -(int)ENOMEM;
    }
    p->stack_page = stack;

#if defined(__m68k__)
    /* m68k user mode: stack_page = kernel stack (SSP).
     * Allocate a separate page for the user stack (USP). */
    void *user_stack = page_alloc();
    if (!user_stack) {
        page_free(stack);
        p->stack_page = NULL;
        vnode_put(vn);
        return -(int)ENOMEM;
    }
#endif

    /* ── XIP: text stays in romfs/flash, data goes to SRAM ─────────── */
    {
        uint32_t xip_text_base =
            (uint32_t)(uintptr_t)file_base + text_seg->p_offset;
        text_base = xip_text_base;

#if defined(__m68k__)
        entry = xip_text_base + e_entry - text_seg->p_vaddr;
#else
        entry = xip_text_base + (e_entry & ~1u) - text_seg->p_vaddr;
        entry |= (e_entry & 1u);   /* restore Thumb bit */
#endif

        if (data_seg && data_seg->p_memsz > 0) {
            uint32_t data_pages =
                (data_seg->p_memsz + PAGE_SIZE - 1) / PAGE_SIZE;
            if (data_pages > USER_PAGES_MAX) {
                page_free(stack);
                p->stack_page = NULL;
                vnode_put(vn);
                return -(int)ENOMEM;
            }

            sram_page = alloc_contiguous(data_pages);
            if (!sram_page) {
                page_free(stack);
                p->stack_page = NULL;
                vnode_put(vn);
                return -(int)ENOMEM;
            }

            /* Copy initialised data */
            if (data_seg->p_filesz > 0)
                memcpy(sram_page, file_base + data_seg->p_offset,
                       data_seg->p_filesz);

            /* Zero BSS */
            if (data_seg->p_memsz > data_seg->p_filesz)
                memset(sram_page + data_seg->p_filesz, 0,
                       data_seg->p_memsz - data_seg->p_filesz);

            /* GOT relocation */
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
                        continue;
                    if (val < data_seg->p_vaddr)
                        got[i] = val + xip_text_base;
                    else
                        got[i] = val - data_seg->p_vaddr +
                                 (uint32_t)(uintptr_t)sram_page;
                }
            }

            /* Apply relocations (data segment only — no text relocs) */
            apply_relocations(ehdr, file_base, file_size,
                              NULL, data_seg, NULL,
                              sram_page, xip_text_base,
                              got_sram_addr, &got_info);

            /* Store data pages in user_pages[] */
            for (uint32_t i = 0; i < data_pages; i++)
                p->user_pages[i] = sram_page + i * PAGE_SIZE;
#if defined(__m68k__)
            /* Track user stack page separately — user_pages[] indices above
             * data_pages are used by sys_brk for heap expansion, so storing
             * the user stack there would get overwritten on first brk. */
            p->user_stack_page = user_stack;
#endif

            /* Set program break — align to 16 bytes for musl malloc */
            uint32_t data_end = (uint32_t)(uintptr_t)sram_page
                                + data_seg->p_memsz;
            data_end = (data_end + 15u) & ~15u;
            p->brk_base    = data_end;
            p->brk_current = data_end;
        }
    }

    /* ── 8a. Build argc/argv/envp/auxv at top of stack ─────────────────
     *
     * musl's _start reads:  argc = [SP], argv = SP+4, ...
     *
     * Layout (high → low):
     *   argv string data (each NUL-terminated)
     *   <alignment padding>
     *   auxv[1] = {AT_NULL, 0}
     *   auxv[0] = {AT_PAGESZ, PAGE_SIZE}
     *   envp[0] = NULL
     *   argv[argc] = NULL
     *   argv[argc-1..0] = pointers to string data
     *   argc
     *              ← user_sp
     */
    {
#if defined(__m68k__)
        uint32_t stack_top = (uint32_t)(uintptr_t)user_stack + PAGE_SIZE;
#else
        uint32_t stack_top = (uint32_t)(uintptr_t)stack + PAGE_SIZE;
#endif
        uint32_t sp = stack_top;

        /* Count arguments */
        int argc = 0;
        if (argv) {
            while (argv[argc] != NULL && argc < 32)
                argc++;
        }
        if (argc == 0) {
            argc = 1;
            argv = NULL;
        }

        /* Copy argument strings to top of stack */
        uint32_t str_addrs[32];
        if (argv) {
            for (int i = argc - 1; i >= 0; i--) {
                uint32_t len = (uint32_t)strlen(argv[i]) + 1;
                sp -= len;
                memcpy((void *)(uintptr_t)sp, argv[i], len);
                str_addrs[i] = sp;
            }
        } else {
            uint32_t len = (uint32_t)strlen(path) + 1;
            sp -= len;
            memcpy((void *)(uintptr_t)sp, path, len);
            str_addrs[0] = sp;
        }

        /* Align stack */
#if defined(__m68k__)
        sp &= ~3u;
#else
        sp &= ~7u;
        if ((argc + 7) & 1)
            sp -= 4;
#endif

        /* auxv[1]: AT_NULL */
        sp -= 4; *(uint32_t *)(uintptr_t)sp = 0;
        sp -= 4; *(uint32_t *)(uintptr_t)sp = 0;
        /* auxv[0]: AT_PAGESZ */
        sp -= 4; *(uint32_t *)(uintptr_t)sp = PAGE_SIZE;
        sp -= 4; *(uint32_t *)(uintptr_t)sp = 6;

        /* envp[0] = NULL */
        sp -= 4; *(uint32_t *)(uintptr_t)sp = 0;

        /* argv array + NULL terminator */
        sp -= 4; *(uint32_t *)(uintptr_t)sp = 0;
        for (int i = argc - 1; i >= 0; i--) {
            sp -= 4; *(uint32_t *)(uintptr_t)sp = str_addrs[i];
        }

        /* argc */
        sp -= 4; *(uint32_t *)(uintptr_t)sp = (uint32_t)argc;

        /* ── 9. Set up the exception frame ─────────────────────────────── */
        proc_setup_stack(p, (void (*)(void))(uintptr_t)entry, sp);
#if defined(__m68k__)
        /* m68k: argc/argv built on user_stack page; set USP there.
         * proc_setup_stack ignores sp on m68k (builds frame on stack_page). */
        p->usp = sp;
#endif
    }

    /* Patch GOT base register in the software frame */
    if (got_sram_addr) {
        uint32_t *sw = (uint32_t *)(uintptr_t)p->sp;
#if defined(__m68k__)
        sw[13] = got_sram_addr;
#else
        sw[5] = got_sram_addr;
#endif
    }

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
