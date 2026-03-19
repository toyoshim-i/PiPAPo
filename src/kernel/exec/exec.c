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
#ifdef PPAP_ENABLE_SOS
#include "exec_sos.h"
#endif
#ifdef PPAP_ENABLE_ECPU_M68K
#include "exec_m68k_emu.h"
#endif
#include "kernel/vfs/vfs.h"
#include "kernel/mm/page.h"
#include "kernel/signal/signal.h"
#include "kernel/errno.h"
#include "arch/arch.h"
#include <string.h>

/* Maximum PT_LOAD segments we handle */
#define MAX_LOAD_SEGS  4

/* ── Contiguous page allocation helper ─────────────────────────────────── */

uint8_t *alloc_contiguous(uint32_t n_pages)
{
    return page_alloc_contiguous(n_pages);
}



/* ── do_execve ─────────────────────────────────────────────────────────── */

#include "loader.h"

int do_execve(pcb_t *p, const char *path, const char *const *argv) {
    vnode_t *vn = NULL;
    int err;

    const char* default_argv[2] = {path, NULL};
    if (!argv || !argv[0]) {
        argv = default_argv;
    }

    /* ── 1. Look up the binary in the VFS ──────────────────────────────── */
    err = vfs_lookup(path, &vn);
    if (err < 0) return err;

    if (vn->type != VNODE_FILE) {
        vnode_put(vn);
        return -(int)ENOEXEC;
    }

    const uint8_t *elf_buf = NULL;
    uint8_t *file_buf = NULL;
    uint32_t file_pages = 0;

    if (vn->xip_addr == NULL) {
        uint32_t file_size = vn->size;
        if (file_size == 0) {
            vnode_put(vn);
            return -(int)ENOEXEC;
        }

        file_pages = (file_size + PAGE_SIZE - 1u) / PAGE_SIZE;
        file_buf = alloc_contiguous(file_pages);
        if (!file_buf) {
            vnode_put(vn);
            return -(int)ENOMEM;
        }

        if (!vn->mount || !vn->mount->ops || !vn->mount->ops->read) {
            for (uint32_t i = 0; i < file_pages; i++) page_free(file_buf + i * PAGE_SIZE);
            vnode_put(vn);
            return -(int)ENOEXEC;
        }

        long nread = vn->mount->ops->read(vn, file_buf, file_size, 0);
        if (nread < 0 || (uint32_t)nread != file_size) {
            for (uint32_t i = 0; i < file_pages; i++) page_free(file_buf + i * PAGE_SIZE);
            vnode_put(vn);
            return (nread < 0) ? (int)nread : -(int)ENOEXEC;
        }

#ifdef PPAP_ENABLE_SOS
        if (sos_detect(path, file_buf, file_size)) {
            int rc = exec_sos(p, file_buf, file_size, path, argv);
            for (uint32_t i = 0; i < file_pages; i++) page_free(file_buf + i * PAGE_SIZE);
            vnode_put(vn);
            return rc;
        }
#endif

#ifdef PPAP_ENABLE_ECPU_M68K
        {
            const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)file_buf;
            if (m68k_emu_detect(ehdr)) {
                int rc = exec_m68k_emu(p, file_buf, file_size, ehdr, path, argv);
                for (uint32_t i = 0; i < file_pages; i++) page_free(file_buf + i * PAGE_SIZE);
                vnode_put(vn);
                return rc;
            }
        }
#endif
        elf_buf = file_buf; 
    }

    const uint8_t *file_base = (elf_buf != NULL) ? elf_buf : (const uint8_t *)vn->xip_addr;
    uint32_t file_size = vn->size;

#ifdef PPAP_ENABLE_SOS
    if (sos_detect(path, file_base, file_size)) {
        vnode_put(vn);
        return exec_sos(p, file_base, file_size, path, argv);
    }
#endif

#ifdef PPAP_ENABLE_ECPU_M68K
    const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)file_base;
    if (m68k_emu_detect(ehdr)) {
        vnode_put(vn);
        return exec_m68k_emu(p, file_base, file_size, ehdr, path, argv);
    }
#endif

    extern const loader_t* loader_registry[];
    const loader_t *matched_loader = NULL;
    int rc = -(int)ENOEXEC;

    for (int i = 0; loader_registry[i] != NULL; i++) {
        if (loader_registry[i]->detect(file_base, file_size, path)) {
            int arch = loader_registry[i]->required_arch_id;
            const cpu_ops_t *cpu_ops;
            if (arch == 0 || arch == HOST_ARCH_ID)
                cpu_ops = &native_cpu_ops;
            else
                cpu_ops = cpu_ops_for_arch(arch);
            if (!cpu_ops) {
                rc = -(int)ENOEXEC;
                break;
            }

            rc = loader_registry[i]->load(p, file_base, file_size, cpu_ops, NULL, argv);
            if (rc == 0)
                matched_loader = loader_registry[i];
            break;
        }
    }

    if (!matched_loader) {
        if (file_buf) {
            for (uint32_t i = 0; i < file_pages; i++) page_free(file_buf + i * PAGE_SIZE);
        }
        vnode_put(vn);
        return rc;
    }

    /* Free file buffer if the loader doesn't need it for XIP */
    if (file_buf && !matched_loader->xip) {
        for (uint32_t i = 0; i < file_pages; i++) page_free(file_buf + i * PAGE_SIZE);
    }

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

    if (current)
        memcpy(p->cwd, current->cwd, sizeof(p->cwd));
    else
        strcpy(p->cwd, "/");

    for (int i = 0; i < NSIG; i++) {
        if (p->sig_handlers[i] != SIG_IGN)
            p->sig_handlers[i] = SIG_DFL;
    }
    p->sig_pending = 0;
    p->sig_blocked = 0;

    vnode_put(vn);
    return 0;
}