/*
 * cpm_bridge.c — CP/M 2.2 BDOS/BIOS bridge (personality layer)
 *
 * Phase 1: BDOS fn 0, 2, 9 (output + exit)
 * Phase 2: BDOS fn 1, 3–8, 10–12 (console I/O) + BIOS console
 *
 * The trap handler intercepts CALL instructions to BDOS (0x0005) and
 * BIOS entry points, translating CP/M API calls to host/PPAP operations.
 *
 * See docs/subsystem-cpm.md §5 for the full design.
 */

#include "cpm_bridge.h"
#include <string.h>

/* ── Platform I/O abstraction ──────────────────────────────────────────── */

#ifdef PPAP_KERNEL

#include "kernel/fd/fd.h"

static void cpm_putchar(uint8_t ch)
{
    extern int sys_write(int fd, const void *buf, int count);
    sys_write(1, &ch, 1);
}

static uint8_t cpm_getchar(void)
{
    extern int sys_read(int fd, void *buf, int count);
    uint8_t ch = 0;
    sys_read(0, &ch, 1);
    return ch;
}

static int cpm_char_ready(void)
{
    extern int sys_read_ready(int fd);
    return sys_read_ready(0);
}

#else /* Host test hooks */

extern void cpm_host_putchar(uint8_t ch);
extern int  cpm_host_getchar(void);
extern int  cpm_host_char_ready(void);

static void cpm_putchar(uint8_t ch)
{
    cpm_host_putchar(ch);
}

static uint8_t cpm_getchar(void)
{
    return (uint8_t)cpm_host_getchar();
}

static int cpm_char_ready(void)
{
    return cpm_host_char_ready();
}

#endif

/* ── BDOS Console I/O functions ────────────────────────────────────────── */

/*
 * Console Input (BDOS function 1)
 * Reads one character, echoes it, returns in A.
 */
static int cpm_console_input(z80_state_t *cpu, cpm_state_t *cpm)
{
    (void)cpu; (void)cpm;
    uint8_t ch = cpm_getchar();
    cpm_putchar(ch);  /* echo */
    return ch;
}

/*
 * Console Output (BDOS function 2)
 * Input:  E = character to print
 */
static void cpm_console_output(z80_state_t *cpu, cpm_state_t *cpm)
{
    (void)cpm;
    cpm_putchar(cpu->e);
}

/*
 * Reader Input (BDOS function 3) — maps to console input
 */
static int cpm_reader_input(z80_state_t *cpu, cpm_state_t *cpm)
{
    (void)cpu; (void)cpm;
    return cpm_getchar();
}

/*
 * Punch Output (BDOS function 4) — maps to console output
 */
static void cpm_punch_output(z80_state_t *cpu, cpm_state_t *cpm)
{
    (void)cpm;
    cpm_putchar(cpu->e);
}

/*
 * List Output (BDOS function 5) — maps to console output
 */
static void cpm_list_output(z80_state_t *cpu, cpm_state_t *cpm)
{
    (void)cpm;
    cpm_putchar(cpu->e);
}

/*
 * Direct Console I/O (BDOS function 6)
 * E=0xFF: input (non-blocking, return char or 0x00)
 * E=0xFE: status check (return 0xFF if ready, 0x00 if not)
 * Otherwise: output E as character
 */
static int cpm_direct_console_io(z80_state_t *cpu, cpm_state_t *cpm)
{
    (void)cpm;
    uint8_t e = cpu->e;

    if (e == 0xFF) {
        /* Non-blocking input */
        if (cpm_char_ready())
            return cpm_getchar();
        return 0x00;
    } else if (e == 0xFE) {
        /* Console status check */
        return cpm_char_ready() ? 0xFF : 0x00;
    } else {
        /* Output */
        cpm_putchar(e);
        return 0;
    }
}

/*
 * Get I/O Byte (BDOS function 7)
 * Returns the IOBYTE at address 0x0003.
 */
static int cpm_get_iobyte(z80_state_t *cpu, cpm_state_t *cpm)
{
    (void)cpm;
    return z80_read8(cpu, 0x0003);
}

/*
 * Set I/O Byte (BDOS function 8)
 * Sets the IOBYTE at address 0x0003 to E.
 */
static void cpm_set_iobyte(z80_state_t *cpu, cpm_state_t *cpm)
{
    (void)cpm;
    z80_write8(cpu, 0x0003, cpu->e);
}

/*
 * Print String (BDOS function 9)
 * Input:  DE = address of '$'-terminated string
 */
static void cpm_print_string(z80_state_t *cpu, cpm_state_t *cpm)
{
    (void)cpm;
    uint16_t addr = z80_de(cpu);

    for (;;) {
        uint8_t ch = z80_read8(cpu, addr);
        if (ch == '$')
            break;
        cpm_putchar(ch);
        addr++;
    }
}

/*
 * Read Console Buffer (BDOS function 10)
 * DE = address of buffer:
 *   buf[0] = max chars (set by caller)
 *   buf[1] = actual chars read (set by BDOS)
 *   buf[2..] = character data
 */
static void cpm_read_console_buffer(z80_state_t *cpu, cpm_state_t *cpm)
{
    (void)cpm;
    uint16_t buf_addr = z80_de(cpu);
    uint8_t max = z80_read8(cpu, buf_addr);
    int n = 0;

    while (n < max) {
        uint8_t ch = cpm_getchar();
        if (ch == '\r' || ch == '\n') {
            cpm_putchar('\r');
            cpm_putchar('\n');
            break;
        }
        if (ch == 0x08 || ch == 0x7F) {
            /* Backspace */
            if (n > 0) {
                n--;
                cpm_putchar(0x08);
                cpm_putchar(' ');
                cpm_putchar(0x08);
            }
            continue;
        }
        /* Echo and store */
        cpm_putchar(ch);
        z80_write8(cpu, buf_addr + 2 + n, ch);
        n++;
    }

    z80_write8(cpu, buf_addr + 1, (uint8_t)n);
}

/*
 * Get Console Status (BDOS function 11)
 * Returns 0xFF if character ready, 0x00 if not.
 */
static int cpm_console_status(z80_state_t *cpu, cpm_state_t *cpm)
{
    (void)cpu; (void)cpm;
    return cpm_char_ready() ? 0xFF : 0x00;
}

/*
 * Return Version Number (BDOS function 12)
 * Returns 0x0022 (CP/M 2.2) in HL.
 */
static int cpm_version_number(z80_state_t *cpu, cpm_state_t *cpm)
{
    (void)cpu; (void)cpm;
    return 0x0022;
}

/* ── BDOS function dispatch ────────────────────────────────────────────── */

static int cpm_bdos_dispatch(z80_state_t *cpu, cpm_state_t *cpm)
{
    uint8_t fn = cpu->c;
    int result = 0;

    switch (fn) {
    case 0:  /* System Reset */
        return ECPU_TRAP_EXIT;

    case 1:  /* Console Input */
        result = cpm_console_input(cpu, cpm);
        break;

    case 2:  /* Console Output */
        cpm_console_output(cpu, cpm);
        break;

    case 3:  /* Reader Input */
        result = cpm_reader_input(cpu, cpm);
        break;

    case 4:  /* Punch Output */
        cpm_punch_output(cpu, cpm);
        break;

    case 5:  /* List Output */
        cpm_list_output(cpu, cpm);
        break;

    case 6:  /* Direct Console I/O */
        result = cpm_direct_console_io(cpu, cpm);
        break;

    case 7:  /* Get I/O Byte */
        result = cpm_get_iobyte(cpu, cpm);
        break;

    case 8:  /* Set I/O Byte */
        cpm_set_iobyte(cpu, cpm);
        break;

    case 9:  /* Print String */
        cpm_print_string(cpu, cpm);
        break;

    case 10: /* Read Console Buffer */
        cpm_read_console_buffer(cpu, cpm);
        break;

    case 11: /* Get Console Status */
        result = cpm_console_status(cpu, cpm);
        break;

    case 12: /* Return Version Number */
        result = cpm_version_number(cpu, cpm);
        break;

    default:
        /* Unknown function — return 0xFF in A */
        result = 0xFF;
        break;
    }

    /* Standard BDOS return convention: result in A and L (HL for fn 12) */
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

    case 2:  /* CONST — console status */
        cpu->a = cpm_char_ready() ? 0xFF : 0x00;
        return ECPU_TRAP_HANDLED;

    case 3:  /* CONIN — console input */
        cpu->a = cpm_getchar();
        return ECPU_TRAP_HANDLED;

    case 4:  /* CONOUT — console output, char in C register */
        cpm_putchar(cpu->c);
        return ECPU_TRAP_HANDLED;

    case 5:  /* LIST — list output, char in C register */
        cpm_putchar(cpu->c);
        return ECPU_TRAP_HANDLED;

    case 6:  /* PUNCH — no-op */
        return ECPU_TRAP_HANDLED;

    case 7:  /* READER — return 0x1A (EOF) */
        cpu->a = 0x1A;
        return ECPU_TRAP_HANDLED;

    case 15: /* LISTST — list status, always ready */
        cpu->a = 0xFF;
        return ECPU_TRAP_HANDLED;

    case 16: /* SECTRAN — sector translate, return BC in HL */
        z80_set_hl(cpu, z80_bc(cpu));
        return ECPU_TRAP_HANDLED;

    default:
        /* Unimplemented BIOS function (disk I/O etc.) — no-op */
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
