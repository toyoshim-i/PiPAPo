/*
 * cpm_bridge.c — CP/M 2.2 BDOS/BIOS bridge (personality layer)
 *
 * Phase 1: minimal BDOS dispatch for Hello World.
 *   Function 0:  System Reset → exit
 *   Function 2:  Console Output → write char
 *   Function 9:  Print String → write until '$'
 *
 * The trap handler intercepts CALL instructions to BDOS (0x0005) and
 * BIOS entry points, translating CP/M API calls to host/PPAP operations.
 *
 * See docs/subsystem-cpm.md §5 for the full design.
 */

#include "cpm_bridge.h"
#include <string.h>

/* ── BDOS function dispatch ────────────────────────────────────────────── */

/*
 * Console Output (BDOS function 2)
 * Input:  E = character to print
 * Output: none
 */
static void cpm_console_output(z80_state_t *cpu, cpm_state_t *cpm)
{
    (void)cpm;
    uint8_t ch = cpu->e;

    /* In kernel context this becomes sys_write(1, &ch, 1).
     * For host tests, we use the write_fn hook if available,
     * otherwise fall back to nothing. */
#ifdef PPAP_KERNEL
    extern int sys_write(int fd, const void *buf, int count);
    sys_write(1, &ch, 1);
#else
    /* Host test hook — set via cpm_state_t or global */
    extern void cpm_host_putchar(uint8_t ch);
    cpm_host_putchar(ch);
#endif
}

/*
 * Print String (BDOS function 9)
 * Input:  DE = address of '$'-terminated string
 * Output: none
 */
static void cpm_print_string(z80_state_t *cpu, cpm_state_t *cpm)
{
    (void)cpm;
    uint16_t addr = z80_de(cpu);

    for (;;) {
        uint8_t ch = z80_read8(cpu, addr);
        if (ch == '$')
            break;
#ifdef PPAP_KERNEL
        extern int sys_write(int fd, const void *buf, int count);
        sys_write(1, &ch, 1);
#else
        extern void cpm_host_putchar(uint8_t ch);
        cpm_host_putchar(ch);
#endif
        addr++;
    }
}

static int cpm_bdos_dispatch(z80_state_t *cpu, cpm_state_t *cpm)
{
    uint8_t fn = cpu->c;
    int result = 0;

    switch (fn) {
    case 0:  /* System Reset */
        return ECPU_TRAP_EXIT;

    case 2:  /* Console Output */
        cpm_console_output(cpu, cpm);
        break;

    case 9:  /* Print String */
        cpm_print_string(cpu, cpm);
        break;

    default:
        /* Unknown function — return 0xFF in A */
        result = 0xFF;
        break;
    }

    /* Standard BDOS return convention: result in A and L */
    cpu->a = result & 0xFF;
    cpu->l = result & 0xFF;
    cpu->h = (result >> 8) & 0xFF;
    return ECPU_TRAP_HANDLED;
}

/* ── BIOS dispatch ─────────────────────────────────────────────────────── */

static int cpm_bios_dispatch(z80_state_t *cpu, cpm_state_t *cpm,
                             int bios_fn)
{
    (void)cpm;

    switch (bios_fn) {
    case 0:  /* BOOT — cold boot → exit */
    case 1:  /* WBOOT — warm boot → exit */
        return ECPU_TRAP_EXIT;

    case 4:  /* CONOUT — console output, char in C register */
        {
            uint8_t ch = cpu->c;
#ifdef PPAP_KERNEL
            extern int sys_write(int fd, const void *buf, int count);
            sys_write(1, &ch, 1);
#else
            extern void cpm_host_putchar(uint8_t ch);
            cpm_host_putchar(ch);
#endif
        }
        return ECPU_TRAP_HANDLED;

    default:
        /* Unimplemented BIOS function — return handled (no-op) */
        return ECPU_TRAP_HANDLED;
    }
}

/* ── Trap handler ──────────────────────────────────────────────────────── */

int cpm_trap_handler(ecpu_state_t *state, int trap_type,
                     uint32_t param, void *ctx)
{
    z80_state_t *cpu = (z80_state_t *)state;
    cpm_state_t *cpm = (cpm_state_t *)ctx;

    if (trap_type == ECPU_TRAP_CALL) {
        /* BDOS entry: CALL 0x0005 */
        if (param == CPM_BDOS_ENTRY)
            return cpm_bdos_dispatch(cpu, cpm);

        /* Warm boot: CALL 0x0000 (JP at address 0 points to BIOS+3) */
        if (param == 0x0000)
            return ECPU_TRAP_EXIT;

        /* BIOS jump table: addresses CPM_BIOS_ENTRY to CPM_BIOS_ENTRY+0x33 */
        if (param >= CPM_BIOS_ENTRY &&
            param < CPM_BIOS_ENTRY + CPM_BIOS_SIZE) {
            int bios_fn = (param - CPM_BIOS_ENTRY) / 3;
            return cpm_bios_dispatch(cpu, cpm, bios_fn);
        }
    }

    if (trap_type == ECPU_TRAP_HALT)
        return ECPU_TRAP_EXIT;

    return ECPU_TRAP_UNHANDLED;
}
