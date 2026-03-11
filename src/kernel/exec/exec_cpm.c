/*
 * exec_cpm.c — CP/M .COM binary loader
 *
 * Detects .COM files by extension, allocates Z80 memory (64KB),
 * loads the .COM binary into emulated memory, sets up the CP/M 2.2
 * environment (FCBs, memory map, BIOS table), and establishes the
 * CP/M subsystem state for emulated execution.
 *
 * See docs/subsystem-cpm.md §4 for the full design.
 */

#include "exec_cpm.h"
#include "exec.h"
#include "kernel/mm/page.h"
#include "kernel/subsys/subsys.h"
#include "kernel/subsys/cpm_bridge.h"
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

/* ── Zero page setup ───────────────────────────────────────────────────── */

static void cpm_setup_zero_page(z80_state_t *cpu, cpm_state_t *cpm)
{
    uint8_t *mem = cpu->memory;

    /* 0x0000: JP BIOS+3 (warm boot entry — trap intercepts CALL 0x0000) */
    mem[0x0000] = 0xC3;  /* JP */
    mem[0x0001] = (CPM_BIOS_ENTRY + 3) & 0xFF;
    mem[0x0002] = (CPM_BIOS_ENTRY + 3) >> 8;

    /* 0x0003: IOBYTE (default: 0x00) */
    mem[0x0003] = 0x00;

    /* 0x0004: current drive/user (high nibble=user, low nibble=drive) */
    mem[0x0004] = (cpm->current_user << 4) | cpm->current_drive;

    /* 0x0005: JP to BDOS entry — the actual dispatch is via trap handler,
     * but the JP target must be a valid address.  We point it at a RET
     * instruction at the BDOS address.  The trap fires on the CALL to 5,
     * not on the JP target. */
    mem[0x0005] = 0xC3;  /* JP */
    mem[0x0006] = (CPM_BIOS_ENTRY - 6) & 0xFF;  /* BDOS entry area */
    mem[0x0007] = (CPM_BIOS_ENTRY - 6) >> 8;

    /* Place a RET at the BDOS target address (safety net — should never
     * execute because the trap intercepts CALL 5 before pushing/jumping) */
    mem[(CPM_BIOS_ENTRY - 6) & 0xFFFF] = 0xC9;  /* RET */
}

/* ── BIOS jump table ───────────────────────────────────────────────────── */

static void cpm_setup_bios_table(z80_state_t *cpu)
{
    uint8_t *mem = cpu->memory;

    /* Write 17 BIOS entries, each is a RET (trap intercepts the CALL) */
    for (int i = 0; i < 17; i++) {
        uint16_t addr = CPM_BIOS_ENTRY + i * 3;
        mem[addr]     = 0xC9;  /* RET */
        mem[addr + 1] = 0x00;
        mem[addr + 2] = 0x00;
    }
}

/* ── FCB parsing ───────────────────────────────────────────────────────── */

static void cpm_parse_fcb(uint8_t *fcb, const char *arg)
{
    /* Initialize FCB to spaces/zeros */
    memset(fcb, ' ', 12);
    memset(fcb + 12, 0, 24);
    fcb[0] = 0;  /* default drive */

    if (!arg || !*arg)
        return;

    /* Check for drive prefix (e.g. "A:") */
    if (arg[1] == ':') {
        char d = arg[0];
        if (d >= 'a' && d <= 'p')
            fcb[0] = d - 'a' + 1;
        else if (d >= 'A' && d <= 'P')
            fcb[0] = d - 'A' + 1;
        arg += 2;
    }

    /* Parse filename (up to 8 chars) */
    int i = 1;
    while (*arg && *arg != '.' && *arg != ' ' && i <= 8) {
        if (*arg == '*') {
            memset(&fcb[i], '?', 9 - i);
            i = 9;
            break;
        }
        fcb[i++] = (*arg >= 'a' && *arg <= 'z') ? *arg - 32 : *arg;
        arg++;
    }

    /* Skip to extension */
    while (*arg && *arg != '.' && *arg != ' ')
        arg++;
    if (*arg == '.')
        arg++;

    /* Parse extension (up to 3 chars) */
    i = 9;
    while (*arg && *arg != ' ' && i <= 11) {
        if (*arg == '*') {
            memset(&fcb[i], '?', 12 - i);
            i = 12;
            break;
        }
        fcb[i++] = (*arg >= 'a' && *arg <= 'z') ? *arg - 32 : *arg;
        arg++;
    }
}

/* ── Command-line tail ─────────────────────────────────────────────────── */

static void cpm_setup_cmdline(z80_state_t *cpu, const char *cmdline)
{
    uint8_t *mem = cpu->memory;

    if (!cmdline || !*cmdline) {
        mem[0x0080] = 0;
        mem[0x0081] = 0;
        return;
    }

    int len = 0;
    const char *p = cmdline;
    while (*p && len < 126) {
        len++;
        p++;
    }

    mem[0x0080] = (uint8_t)len;
    memcpy(&mem[0x0081], cmdline, len);
    mem[0x0081 + len] = 0x00;
}

/* ── Binary loader ─────────────────────────────────────────────────────── */

static void cpm_load_com(z80_state_t *cpu, cpm_state_t *cpm,
                         const uint8_t *binary, uint32_t size,
                         const char *cmdline)
{
    /* Clamp size to TPA limits */
    if (size > CPM_MAX_COM_SIZE)
        size = CPM_MAX_COM_SIZE;

    /* Zero all emulated memory */
    memset(cpu->memory, 0, cpu->mem_size);

    /* Initialize CP/M state defaults */
    cpm->current_drive = 0;  /* A: */
    cpm->current_user = 0;
    cpm->dma_addr = CPM_DMA_DEFAULT;
    memset(cpm->open_files, 0, sizeof(cpm->open_files));

    /* Set up memory map */
    cpm_setup_zero_page(cpu, cpm);
    cpm_setup_bios_table(cpu);

    /* Load .COM binary at 0x0100 */
    memcpy(&cpu->memory[CPM_TPA_BASE], binary, size);

    /* Parse command line into FCBs and DMA tail */
    if (cmdline && *cmdline) {
        /* Find first and second arguments for FCB1/FCB2 */
        const char *arg1 = cmdline;
        while (*arg1 == ' ') arg1++;

        const char *arg2 = arg1;
        while (*arg2 && *arg2 != ' ') arg2++;
        while (*arg2 == ' ') arg2++;

        cpm_parse_fcb(&cpu->memory[CPM_FCB1_ADDR], arg1);
        cpm_parse_fcb(&cpu->memory[CPM_FCB2_ADDR],
                       (*arg2) ? arg2 : NULL);
    } else {
        cpm_parse_fcb(&cpu->memory[CPM_FCB1_ADDR], NULL);
        cpm_parse_fcb(&cpu->memory[CPM_FCB2_ADDR], NULL);
    }

    cpm_setup_cmdline(cpu, cmdline);

    /* Set initial CPU state */
    cpu->pc = CPM_TPA_BASE;          /* execution starts at 0x0100 */
    cpu->sp = CPM_TPA_END;           /* stack below BIOS area */

    /* Push return address 0x0000 so RET from .COM triggers warm boot */
    z80_push16(cpu, 0x0000);
}

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
