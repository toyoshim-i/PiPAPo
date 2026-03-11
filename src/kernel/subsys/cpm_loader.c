/*
 * cpm_loader.c — CP/M .COM binary loader
 *
 * Sets up the CP/M 2.2 memory map in emulated Z80 memory:
 *   0x0000: JP to warm boot (BIOS+3)
 *   0x0003: IOBYTE
 *   0x0004: current drive/user
 *   0x0005: JP to BDOS entry
 *   0x005C: default FCB 1
 *   0x006C: default FCB 2
 *   0x0080: command-line tail (DMA buffer)
 *   0x0100: .COM binary loaded here
 *   0xFE00: BIOS jump table (RET stubs — traps intercept the CALL)
 *
 * See docs/subsystem-cpm.md §4 for the full design.
 */

#include "cpm_bridge.h"
#include <string.h>

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

/* ── Public loader entry point ─────────────────────────────────────────── */

void cpm_load_com(z80_state_t *cpu, cpm_state_t *cpm,
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
