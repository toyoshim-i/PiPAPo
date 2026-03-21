/*
 * com_loader.c — CP/M .COM binary format loader
 *
 * Detects .COM files by extension and loads them into a Z80 emulator
 * instance for execution.  Memory allocation, Z80 initialization, and
 * subsystem setup are coordinated here; the actual binary loading logic
 * is delegated to cpm_loader.c.
 *
 * See docs/subsystems/cpm.md §4 for the full design.
 */

#include "com_loader.h"
#include "exec.h"
#include "kernel/mm/page.h"
#include "kernel/subsys/subsys.h"
#include "kernel/subsys/cpm_bridge.h"
#include "kernel/subsys/cpm_loader.h"
#include "kernel/cpu/ecpu_z80.h"
#include "kernel/errno.h"
#if defined(__m68k__)
#include "arch/ioregs.h"
#endif
#include <string.h>

/* Z80 address space: 64KB = 16 × 4KB pages */
#define Z80_MEM_PAGES  16

/* ── Detection ─────────────────────────────────────────────────────────── */

static int com_detect(const uint8_t *file_buf, uint32_t file_size,
                      const char *path)
{
    (void)file_buf;

    if (file_size > CPM_MAX_COM_SIZE || file_size == 0)
        return 0;

    /* Check for .COM or .com extension */
    size_t len = strlen(path);
    if (len < 5)
        return 0;

    const char *ext = path + len - 4;
    if ((ext[0] == '.') &&
        (ext[1] == 'C' || ext[1] == 'c') &&
        (ext[2] == 'O' || ext[2] == 'o') &&
        (ext[3] == 'M' || ext[3] == 'm'))
        return 1;

    return 0;
}

/* ── Helpers ───────────────────────────────────────────────────────────── */

/*
 * Per-process CP/M execution state -- stored in subsys_data.
 * Allocated from a separate page since it doesn't fit in Z80 address space.
 */
typedef struct {
    z80_state_t  z80;
    cpm_state_t  cpm;
} cpm_exec_state_t;

_Static_assert(sizeof(cpm_exec_state_t) <= PAGE_SIZE,
               "cpm_exec_state_t must fit in one page");

static void cpm_set_drive_a_root(cpm_state_t *cpm, const char *path)
{
    const char *slash = NULL;
    uint32_t len = 0;

    cpm->drive_a_root[0] = 0;
    if (!path || !*path)
        return;

    for (const char *s = path; *s; s++) {
        if (*s == '/')
            slash = s;
    }

    if (!slash)
        return;
    if (slash == path) {
        cpm->drive_a_root[0] = '/';
        cpm->drive_a_root[1] = 0;
        return;
    }

    len = (uint32_t)(slash - path);
    if (len >= sizeof(cpm->drive_a_root))
        len = sizeof(cpm->drive_a_root) - 1;
    memcpy(cpm->drive_a_root, path, len);
    cpm->drive_a_root[len] = 0;
}

/* ── Loader ────────────────────────────────────────────────────────────── */

static int com_load(pcb_t *p, const uint8_t *file_buf, uint32_t file_size,
                    const cpu_ops_t *cpu_ops, void *cpu_state,
                    const char *const *argv)
{
    (void)cpu_ops;
    (void)cpu_state;

    /* ── 1. Allocate Z80 memory (64KB contiguous) + separate state page ── */
    uint8_t *mem_base = alloc_contiguous(Z80_MEM_PAGES);
    if (!mem_base)
        return -(int)ENOMEM;

    uint8_t *z80_mem = mem_base;

    uint8_t *state_page = page_alloc();
    if (!state_page) {
        for (uint32_t i = 0; i < Z80_MEM_PAGES; i++)
            page_free(mem_base + i * PAGE_SIZE);
        return -(int)ENOMEM;
    }

    cpm_exec_state_t *state = (cpm_exec_state_t *)state_page;

    /* Store pages in user_pages[] for cleanup on exit */
    for (uint32_t i = 0; i < Z80_MEM_PAGES && i < USER_PAGES_MAX; i++)
        p->user_pages[i] = mem_base + i * PAGE_SIZE;
    if (Z80_MEM_PAGES < USER_PAGES_MAX)
        p->user_pages[Z80_MEM_PAGES] = state_page;

    /* ── 2. Allocate stack page ────────────────────────────────────────── */
    void *stack = page_alloc();
    if (!stack) {
        for (uint32_t i = 0; i < Z80_MEM_PAGES; i++)
            page_free(mem_base + i * PAGE_SIZE);
        page_free(state_page);
        return -(int)ENOMEM;
    }
    p->stack_page = stack;

    /* ── 3. Initialize Z80 emulator ────────────────────────────────────── */
    memset(state, 0, sizeof(*state));
    ecpu_z80_ops.init((cpu_state_t *)&state->z80, z80_mem, 65536);

    /* Set up trap handler for BDOS/BIOS interception */
    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&state->z80,
                                   cpm_trap_handler, &state->cpm);

    /* ── 4. Build command line from argv ───────────────────────────────── */
    char cmdline[128];
    cmdline[0] = '\0';
    if (argv && argv[0]) {
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
    /* Extract path from argv[0] for drive_a_root */
    const char *path = (argv && argv[0]) ? argv[0] : "";
    cpm_load_com(&state->z80, &state->cpm, file_buf, file_size, cmdline);
    cpm_set_drive_a_root(&state->cpm, path);

    /* ── 6. Tag as CP/M process ────────────────────────────────────────── */
    p->subsys = SUBSYS_CPM;
    p->subsys_data = state;

    {
        const subsys_ops_t *ops = subsys_ops_table[SUBSYS_CPM];
        if (ops && ops->on_init)
            ops->on_init(p);
    }

    /* ── 7. Set up kernel-mode entry point ─────────────────────────────
     *
     * Unlike ELF which runs in user mode, CP/M .COM programs run via
     * the Z80 emulator in kernel mode.  proc_setup_stack() sets the
     * stack frame so the scheduler will "return" into cpm_run_process.
     */
    proc_setup_stack(p, cpm_run_process, 0);

#if defined(__m68k__)
    /* CP/M runs the Z80 emulator in kernel context on m68k.
     * Force the initial RTE frame to supervisor mode so low-level
     * helpers that touch SR (irq save/restore) do not trap. */
    {
        uint8_t *exc = (uint8_t *)(uintptr_t)p->sp + 15u * sizeof(uint32_t);
        *(uint16_t *)(void *)exc = SR_SUPV_IRQ;
        p->usp = (uint32_t)(uintptr_t)p->stack_page + PAGE_SIZE;
    }
#endif

    return 0;
}

/* ── Loader registration ───────────────────────────────────────────────── */

const loader_t com_loader = {
    .name = "com",
    .detect = com_detect,
    .load = com_load,
    .required_arch_id = CPU_ARCH_Z80,
    .xip = 0,
};
