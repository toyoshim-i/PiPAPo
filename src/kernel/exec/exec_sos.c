/*
 * exec_sos.c — S-OS "SWORD" .obj binary loader
 *
 * Detects _SOS magic header and orchestrates loading of S-OS binaries
 * into a Z80 emulator instance.  Memory allocation, Z80 initialization,
 * and subsystem setup are coordinated here.
 *
 * See docs/proposals/sos_subsystem.md §5 for the format.
 */

#include "exec_sos.h"
#include "exec.h"
#include "kernel/mm/page.h"
#include "kernel/subsys/subsys.h"
#include "kernel/subsys/sos_bridge.h"
#include "kernel/ecpu/ecpu_z80.h"
#include "kernel/signal/signal.h"
#include "kernel/errno.h"
#if defined(__m68k__)
#include "arch/cpu.h"
#endif
#include <string.h>

/* Z80 address space: 64KB = 16 × 4KB pages */
#define Z80_MEM_PAGES  16

/* ── Detection ─────────────────────────────────────────────────────────── */

int sos_detect(const char *path, const uint8_t *file, uint32_t size)
{
    (void)path;

    /* Must have at least the 18-byte _SOS header */
    if (size < SOS_HEADER_SIZE)
        return 0;

    /* Check _SOS magic */
    if (memcmp(file, SOS_MAGIC, SOS_MAGIC_LEN) != 0)
        return 0;

    /* Validate header structure: spaces at +4, +7, +12 and LF at +17 */
    if (file[4] != ' ' || file[7] != ' ' || file[12] != ' ')
        return 0;
    if (file[17] != 0x0A)
        return 0;

    return 1;
}

/* ── Per-process S-OS execution state ─────────────────────────────────── */

typedef struct {
    z80_state_t  z80;
    sos_state_t  sos;
} sos_exec_state_t;

_Static_assert(sizeof(sos_exec_state_t) <= PAGE_SIZE,
               "sos_exec_state_t must fit in one page");

/* ── S-OS memory map initialization ───────────────────────────────────── */

static void sos_setup_memory(z80_state_t *cpu, sos_state_t *sos)
{
    uint8_t *mem = cpu->memory;

    /* RST 00h at 0x0000: JP to cold start — trap intercepts */
    mem[0x0000] = 0xC3;  /* JP */
    mem[0x0001] = SOS_COLD_ENTRY & 0xFF;
    mem[0x0002] = SOS_COLD_ENTRY >> 8;

    /* RST 18h at 0x0018: JP to self — trap intercepts before execution */
    mem[SOS_RST18_ADDR]     = 0xC3;  /* JP */
    mem[SOS_RST18_ADDR + 1] = SOS_RST18_ADDR & 0xFF;
    mem[SOS_RST18_ADDR + 2] = SOS_RST18_ADDR >> 8;

    /*
     * Populate monitor jump table (0x1F80–0x1FFD) and extended API
     * (0x2000–0x2036) with executable stubs: { RST 0; RET; NOP }.
     *
     * Programs may reach these entries via JP (not just CALL), so the
     * entries must contain real code.  RST 0 fires ECPU_TRAP_CALL(0x0000);
     * the trap handler identifies the function from cpu->pc (which points
     * just past the RST byte, i.e. inside the 3-byte entry).  On return
     * from the trap, the RET instruction pops the original caller's
     * return address.
     */
    for (uint16_t addr = SOS_MON_BASE; addr <= SOS_MON_TOP; addr += 3) {
        mem[addr]     = 0xC7;  /* RST 0 */
        mem[addr + 1] = 0xC9;  /* RET */
        mem[addr + 2] = 0x00;  /* NOP (padding) */
    }
    for (uint16_t addr = SOS_EXT_BASE; addr <= SOS_EXT_TOP; addr += 3) {
        mem[addr]     = 0xC7;  /* RST 0 */
        mem[addr + 1] = 0xC9;  /* RET */
        mem[addr + 2] = 0x00;  /* NOP (padding) */
    }

    /* Initialize work area (0x1F5B–0x1F7F) */
    mem[SOS_MXLIN]  = 25;             /* screen height */
    mem[SOS_WIDTH]  = 80;             /* screen width */
    mem[SOS_DSK]    = sos->current_session;  /* current device */
    mem[SOS_DVSW]   = 0;              /* device: FDD */
    mem[SOS_LPSW]   = 0;              /* printer off */

    /* User startup address → #HOT */
    mem[SOS_USR]     = 0xFA;  /* 0x1FFA = #HOT */
    mem[SOS_USR + 1] = 0x1F;

    /* Stack pointer */
    mem[SOS_STKAD]     = SOS_STACK_TOP & 0xFF;
    mem[SOS_STKAD + 1] = SOS_STACK_TOP >> 8;

    /* User RAM limit */
    mem[SOS_MEMAX]     = 0x00;
    mem[SOS_MEMAX + 1] = 0xD0;  /* D000h */

    /* Initialize file_fd */
    sos->file_fd = -1;

    /* Initialize screen state */
    sos->screen_width  = 80;
    sos->screen_height = 25;
    sos->cursor_x = 0;
    sos->cursor_y = 0;
}

/* ── Drive root mapping ───────────────────────────────────────────────── */

static void sos_set_session_root(sos_state_t *sos, const char *path)
{
    (void)sos;
    (void)path;
    /* S-OS uses session-based path mapping (/a/, /b/) — no per-binary
     * root override needed (unlike CP/M's drive A: root). */
}

/* ── Loader ───────────────────────────────────────────────────────────── */

int exec_sos(pcb_t *p, const uint8_t *file, uint32_t size,
             const char *path, const char *const *argv)
{
    (void)argv;

    /* ── 1. Parse _SOS header ──────────────────────────────────────────── */
    sos_header_t hdr;
    if (sos_parse_header(file, size, &hdr) < 0)
        return -(int)ENOEXEC;

    /* Only binary mode is executable */
    if (hdr.file_mode != SOS_MODE_BINARY)
        return -(int)ENOEXEC;

    /* Payload is everything after the 18-byte header */
    const uint8_t *payload = file + SOS_HEADER_SIZE;
    uint32_t payload_size = size - SOS_HEADER_SIZE;

    /* Validate load address */
    if (hdr.load_addr + payload_size > 65536)
        payload_size = 65536 - hdr.load_addr;

    /* ── 2. Allocate Z80 memory (64KB) + state page ────────────────────── */
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

    sos_exec_state_t *state = (sos_exec_state_t *)state_page;

    /* Store pages in user_pages[] for cleanup on exit */
    for (uint32_t i = 0; i < Z80_MEM_PAGES && i < USER_PAGES_MAX; i++)
        p->user_pages[i] = mem_base + i * PAGE_SIZE;
    if (Z80_MEM_PAGES < USER_PAGES_MAX)
        p->user_pages[Z80_MEM_PAGES] = state_page;

    /* ── 3. Allocate stack page ────────────────────────────────────────── */
    void *stack = page_alloc();
    if (!stack) {
        for (uint32_t i = 0; i < Z80_MEM_PAGES; i++)
            page_free(mem_base + i * PAGE_SIZE);
        page_free(state_page);
        return -(int)ENOMEM;
    }
    p->stack_page = stack;

    /* ── 4. Initialize Z80 emulator ────────────────────────────────────── */
    memset(state, 0, sizeof(*state));
    ecpu_z80_ops.init((ecpu_state_t *)&state->z80, z80_mem, 65536);

    /* Set up trap handler for RST 18h / CALL interception */
    ecpu_z80_ops.set_trap_handler((ecpu_state_t *)&state->z80,
                                   sos_trap_handler, &state->sos);

    /* ── 5. Zero memory and set up S-OS memory map ─────────────────────── */
    memset(z80_mem, 0, 65536);
    sos_setup_memory(&state->z80, &state->sos);

    /* ── 6. Load payload at header-specified load address ──────────────── */
    memcpy(&z80_mem[hdr.load_addr], payload, payload_size);

    /* Set initial CPU state */
    state->z80.pc = hdr.exec_addr;
    state->z80.sp = SOS_STACK_TOP;  /* Stack at 0x0800, grows down */

    /* Push return address 0x0000 so RET triggers cold start (exit) */
    z80_push16(&state->z80, 0x0000);

    /* Set DTADR/SIZE/EXADR in work area */
    z80_mem[SOS_DTADR]     = hdr.load_addr & 0xFF;
    z80_mem[SOS_DTADR + 1] = hdr.load_addr >> 8;
    z80_mem[SOS_SIZE]      = payload_size & 0xFF;
    z80_mem[SOS_SIZE + 1]  = (payload_size >> 8) & 0xFF;
    z80_mem[SOS_EXADR]     = hdr.exec_addr & 0xFF;
    z80_mem[SOS_EXADR + 1] = hdr.exec_addr >> 8;

    sos_set_session_root(&state->sos, path);

    /* ── 7. Tag as S-OS process ────────────────────────────────────────── */
    p->subsys = SUBSYS_SOS;
    p->subsys_data = state;

    {
        const subsys_ops_t *ops = subsys_ops_table[SUBSYS_SOS];
        if (ops && ops->on_init)
            ops->on_init(p);
    }

    /* ── 8. Set process comm from executable basename ──────────────────── */
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

    /* ── 9. Set working directory ──────────────────────────────────────── */
    if (current)
        memcpy(p->cwd, current->cwd, sizeof(p->cwd));
    else
        strcpy(p->cwd, "/");

    /* ── 10. Reset signal state ────────────────────────────────────────── */
    for (int i = 0; i < NSIG; i++) {
        if (p->sig_handlers[i] != SIG_IGN)
            p->sig_handlers[i] = SIG_DFL;
    }
    p->sig_pending = 0;
    p->sig_blocked = 0;

    /* ── 11. Set up kernel-mode entry point ─────────────────────────────── */
    proc_setup_stack(p, sos_run_process, 0);

#if defined(__m68k__)
    {
        uint8_t *exc = (uint8_t *)(uintptr_t)p->sp + 15u * sizeof(uint32_t);
        *(uint16_t *)(void *)exc = SR_SUPV_IRQ;
        p->usp = (uint32_t)(uintptr_t)p->stack_page + PAGE_SIZE;
    }
#endif

    return 0;
}
