/*
 * exec_m68k_emu.c — m68k ELF binary loader for cross-arch emulation
 *
 * Detects m68k ELF binaries on non-m68k hosts, allocates emulated
 * memory, loads PT_LOAD segments, and sets up the eCPU-m68k state
 * for execution via the PPAP cross-arch personality.
 *
 * Pattern follows exec_cpm.c: allocate memory → init eCPU →
 * set trap handler → tag process → call on_init.
 */

#include "exec_m68k_emu.h"
#include "exec.h"
#include "kernel/mm/page.h"
#include "kernel/subsys/subsys.h"
#include "kernel/subsys/ppap_m68k_bridge.h"
#include "kernel/cpu/ecpu_m68k.h"
#include "kernel/signal/signal.h"
#include "kernel/endian.h"
#include "kernel/errno.h"
#include <string.h>

/* Emulated memory: 1MB = 256 × 4KB pages */
#define M68K_EMU_MEM_SIZE   (1 << 20)
#define M68K_EMU_MEM_PAGES  (M68K_EMU_MEM_SIZE / PAGE_SIZE)

/* Default stack top in emulated address space */
#define M68K_EMU_STACK_TOP  0x00080000u

/* ── Detection ─────────────────────────────────────────────────────────── */

int m68k_emu_detect(const elf32_ehdr_t *ehdr)
{
#if defined(__m68k__)
    /* Native m68k — no emulation needed */
    (void)ehdr;
    return 0;
#else
    /* Check for m68k ELF on non-m68k host */
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3)
        return 0;
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32)
        return 0;
    if (ehdr->e_ident[EI_DATA] != ELFDATA2MSB)
        return 0;

    /* Read e_machine — it's big-endian in the file */
    const uint8_t *raw = (const uint8_t *)&ehdr->e_machine;
    uint16_t machine = ((uint16_t)raw[0] << 8) | raw[1];
    return machine == EM_68K;
#endif
}

/* ── Loader ────────────────────────────────────────────────────────────── */

int exec_m68k_emu(pcb_t *p, const uint8_t *file_base, uint32_t file_size,
                   const elf32_ehdr_t *ehdr, const char *path,
                   const char *const *argv)
{
    int exec_argc = 1;
    int use_default_argv0 = 1;

    if (argv && argv[0]) {
        int argc = 0;
        while (argv[argc] != NULL) {
            argc++;
            if (argc > EXEC_ARGV_MAX)
                return -(int)E2BIG;
        }
        exec_argc = argc;
        use_default_argv0 = 0;
    }

    /* ── 1. Allocate emulated memory + state struct ────────────────────── */
    uint32_t state_pages = (sizeof(ppap_m68k_exec_state_t) + PAGE_SIZE - 1)
                           / PAGE_SIZE;
    uint32_t total_pages = M68K_EMU_MEM_PAGES + state_pages;
    uint8_t *mem_base = alloc_contiguous(total_pages);
    if (!mem_base)
        return -(int)ENOMEM;

    uint8_t *emu_mem = mem_base;
    ppap_m68k_exec_state_t *state =
        (ppap_m68k_exec_state_t *)(mem_base + M68K_EMU_MEM_PAGES * PAGE_SIZE);

    /* Store pages for cleanup on exit */
    for (uint32_t i = 0; i < total_pages && i < USER_PAGES_MAX; i++)
        p->user_pages[i] = mem_base + i * PAGE_SIZE;

    /* ── 2. Allocate stack page ────────────────────────────────────────── */
    void *stack = page_alloc();
    if (!stack) {
        for (uint32_t i = 0; i < total_pages; i++)
            page_free(mem_base + i * PAGE_SIZE);
        return -(int)ENOMEM;
    }
    p->stack_page = stack;

    /* ── 3. Initialize m68k emulator ───────────────────────────────────── */
    memset(state, 0, sizeof(*state));
    ecpu_m68k_ops.init((cpu_state_t *)&state->m68k, emu_mem, M68K_EMU_MEM_SIZE);

    /* Set trap handler for PPAP syscall interception */
    ecpu_m68k_ops.set_trap_handler((cpu_state_t *)&state->m68k,
                                    ppap_m68k_trap_handler, NULL);

    /* ── 4. Load PT_LOAD segments into emulated memory ─────────────────── */
    /* Parse program headers (big-endian) */
    uint32_t phoff = be32_load(&ehdr->e_phoff);
    uint16_t phnum = be16_load(&ehdr->e_phnum);
    uint16_t phentsize = be16_load(&ehdr->e_phentsize);

    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *ph = file_base + phoff + i * phentsize;
        uint32_t p_type   = be32_load(ph + 0);
        uint32_t p_offset = be32_load(ph + 4);
        uint32_t p_vaddr  = be32_load(ph + 8);
        uint32_t p_filesz = be32_load(ph + 16);
        uint32_t p_memsz  = be32_load(ph + 20);

        if (p_type != PT_LOAD)
            continue;

        /* Bounds check */
        if (p_vaddr + p_memsz > M68K_EMU_MEM_SIZE)
            continue;
        if (p_offset + p_filesz > file_size)
            continue;

        /* Copy file data — already big-endian, m68k memory is big-endian */
        if (p_filesz > 0)
            memcpy(emu_mem + p_vaddr, file_base + p_offset, p_filesz);

        /* Zero BSS */
        if (p_memsz > p_filesz)
            memset(emu_mem + p_vaddr + p_filesz, 0, p_memsz - p_filesz);
    }

    /* ── 5. Set entry point ────────────────────────────────────────────── */
    uint32_t entry = be32_load(&ehdr->e_entry);
    state->m68k.pc = entry;

    /* ── 6. Set up user stack with argc/argv (big-endian) ──────────────── */
    {
        uint32_t sp = M68K_EMU_STACK_TOP;

        int argc = exec_argc;

        /* Copy argument strings to stack */
        uint32_t str_addrs[EXEC_ARGV_MAX];
        if (!use_default_argv0) {
            for (int i = argc - 1; i >= 0; i--) {
                uint32_t len = (uint32_t)strlen(argv[i]) + 1;
                sp -= len;
                memcpy(emu_mem + sp, argv[i], len);
                str_addrs[i] = sp;
            }
        } else {
            uint32_t len = (uint32_t)strlen(path) + 1;
            sp -= len;
            memcpy(emu_mem + sp, path, len);
            str_addrs[0] = sp;
        }

        /* Align to 4 bytes */
        sp &= ~3u;

        /* envp[0] = NULL */
        sp -= 4;
        m68k_write32(&state->m68k, sp, 0);

        /* argv[argc] = NULL */
        sp -= 4;
        m68k_write32(&state->m68k, sp, 0);

        /* argv[argc-1..0] */
        for (int i = argc - 1; i >= 0; i--) {
            sp -= 4;
            m68k_write32(&state->m68k, sp, str_addrs[i]);
        }

        /* argc */
        sp -= 4;
        m68k_write32(&state->m68k, sp, (uint32_t)argc);

        state->m68k.a[7] = sp;
    }

    /* ── 7. Set supervisor mode for kernel execution ───────────────────── */
    state->m68k.sr = M68K_SR_S;

    /* ── 8. Tag as PPAP process with eCPU m68k state ───────────────────── */
    p->subsys = SUBSYS_PPAP;
    p->subsys_data = state;

    /* ── 9. Set process comm from executable basename ──────────────────── */
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

    /* ── 10. Set working directory ─────────────────────────────────────── */
    if (current)
        memcpy(p->cwd, current->cwd, sizeof(p->cwd));
    else
        strcpy(p->cwd, "/");

    /* ── 11. Reset signal state ────────────────────────────────────────── */
    for (int i = 0; i < NSIG; i++) {
        if (p->sig_handlers[i] != SIG_IGN)
            p->sig_handlers[i] = SIG_DFL;
    }
    p->sig_pending = 0;
    p->sig_blocked = 0;

    /* ── 12. Set kernel-mode entry point ───────────────────────────────── */
    proc_setup_stack(p, ppap_m68k_run_process, 0);

    return 0;
}
