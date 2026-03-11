/*
 * exec_cpm.c — CP/M .COM binary loader
 *
 * Detects .COM files by extension and orchestrates loading of .COM binaries
 * into a Z80 emulator instance. Memory allocation, Z80 initialization, and
 * subsystem setup are coordinated here; the actual binary loading logic
 * is delegated to cpm_loader.c.
 *
 * See docs/subsystem-cpm.md §4 for the full design.
 */

#include "exec_cpm.h"
#include "exec.h"
#include "kernel/mm/page.h"
#include "kernel/subsys/subsys.h"
#include "kernel/subsys/cpm_bridge.h"
#include "kernel/subsys/cpm_loader.h"
#include "kernel/ecpu/ecpu_z80.h"
#include "kernel/signal/signal.h"
#include "kernel/errno.h"
#include <string.h>

/* Z80 address space: 64KB = 16 × 4KB pages */
#define Z80_MEM_PAGES  16

/* ── Detection ─────────────────────────────────────────────────────────── */

int cpm_detect(const char *path, const uint8_t *file, uint32_t size)
{
    (void)file;

    if (size > CPM_MAX_COM_SIZE || size == 0)
        return 0;

    /* Check for .COM or .com extension */
    size_t len = strlen(path);
    if (len < 5)
        return 0;

    const char *ext = path + len - 4;
    if ((ext[0] == '.' || ext[0] == '.') &&
        (ext[1] == 'C' || ext[1] == 'c') &&
        (ext[2] == 'O' || ext[2] == 'o') &&
        (ext[3] == 'M' || ext[3] == 'm'))
        return 1;

    return 0;
}

/* ── Loader ────────────────────────────────────────────────────────────── */

/* ── Execution setup ───────────────────────────────────────────────────── */

/*
 * Per-process CP/M execution state -- stored in subsys_data.
 * Allocated from the Z80 memory pages (placed after the 64KB).
 *
 * Actually, we allocate one extra page for z80_state_t + cpm_state_t
 * since they don't fit in the Z80 address space.
 */
typedef struct {
    z80_state_t  z80;
    cpm_state_t  cpm;
} cpm_exec_state_t;

int exec_cpm(pcb_t *p, const uint8_t *file, uint32_t size,
             const char *path, const char *const *argv)
{
    (void)argv;

    /* ── 1. Allocate Z80 memory (64KB) + state page ────────────────────── */
    uint32_t total_pages = Z80_MEM_PAGES + 1;  /* +1 for state structs */
    uint8_t *mem_base = alloc_contiguous(total_pages);
    if (!mem_base)
        return -(int)ENOMEM;

    /* Z80 memory is the first 16 pages */
    uint8_t *z80_mem = mem_base;

    /* State structs live in the extra page */
    cpm_exec_state_t *state =
        (cpm_exec_state_t *)(mem_base + Z80_MEM_PAGES * PAGE_SIZE);

    /* Store pages in user_pages[] for cleanup on exit */
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

    /* ── 3. Initialize Z80 emulator ────────────────────────────────────── */
    memset(state, 0, sizeof(*state));
    ecpu_z80_ops.init((ecpu_state_t *)&state->z80, z80_mem, 65536);

    /* Set up trap handler for BDOS/BIOS interception */
    ecpu_z80_ops.set_trap_handler((ecpu_state_t *)&state->z80,
                                   cpm_trap_handler, &state->cpm);

    /* ── 4. Build command line from argv ───────────────────────────────── */
    char cmdline[128];
    cmdline[0] = '\0';
    if (argv && argv[0]) {
        /* Skip argv[0] (program name), concatenate the rest */
        int pos = 0;
        for (int i = 1; argv[i] && pos < 126; i++) {
            if (i > 1 && pos < 126)
                cmdline[pos++] = ' ';
            size_t alen = strlen(argv[i]);
            if (pos + (int)alen > 126)
                alen = 126 - pos;
            memcpy(cmdline + pos, argv[i], alen);
            pos += (int)alen;
        }
        cmdline[pos] = '\0';
    }

    /* ── 5. Load .COM binary into Z80 memory ──────────────────────────── */
    cpm_load_com(&state->z80, &state->cpm, file, size, cmdline);

    /* ── 6. Tag as CP/M process ────────────────────────────────────────── */
    p->subsys = SUBSYS_CPM;
    p->subsys_data = state;

    {
        const subsys_ops_t *ops = subsys_ops_table[SUBSYS_CPM];
        if (ops && ops->on_init)
            ops->on_init(p);
    }

    /* ── 7. Set process comm from executable basename ──────────────────── */
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

    /* ── 8. Set working directory ──────────────────────────────────────── */
    if (current)
        memcpy(p->cwd, current->cwd, sizeof(p->cwd));
    else
        strcpy(p->cwd, "/");

    /* ── 9. Reset signal state ─────────────────────────────────────────── */
    for (int i = 0; i < NSIG; i++) {
        if (p->sig_handlers[i] != SIG_IGN)
            p->sig_handlers[i] = SIG_DFL;
    }
    p->sig_pending = 0;
    p->sig_blocked = 0;

    /* ── 10. Set up kernel-mode entry point ─────────────────────────────
     *
     * Unlike ELF/X68k which run in user mode, CP/M .COM programs run
     * via the Z80 emulator in kernel mode.  The process entry point is
     * cpm_run_process(), which calls ecpu_z80_ops.run() in a loop.
     *
     * proc_setup_stack() sets the stack frame so the scheduler will
     * "return" into our entry function.
     */
    proc_setup_stack(p, cpm_run_process, 0);

    return 0;
}
