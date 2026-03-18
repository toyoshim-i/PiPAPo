/*
 * sos_bridge.c — S-OS "SWORD" API bridge (personality layer)
 *
 * Intercepts RST 18h and CALL 1FFBh/1FFEh from Z80-emulated S-OS programs
 * and translates S-OS system calls into PPAP syscalls.
 *
 * Phase S-1: Console output (#COLD, #HOT, #VER, #PRINT, #MSG, #MSX,
 *            #TAB, #CR, #LF, #BELL, #PRTHX, #PRTHL)
 * Phase S-2: Console input (#GETL, #GETKY, #BRKEY, #INKEY, #PAUSE)
 * Phase S-3: File operations (#FILE, #FSAVE, #FLOAD, #FVRFY, #FKILL, #FREN)
 * Phase S-4: Numeric conversion (#ASC1B, #HEX1B, #HEX2B) + stubs
 *
 * See docs/proposals/sos_subsystem.md for the design.
 */

#include "sos_bridge.h"
#include "common/ptrace.h"
#include <string.h>

/* ── _SOS header parsing ───────────────────────────────────────────────── */

static int hex_digit(uint8_t ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

static int hex2byte(const uint8_t *p)
{
    int hi = hex_digit(p[0]);
    int lo = hex_digit(p[1]);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

static int hex4_to_u16(const uint8_t *p, uint16_t *out)
{
    int b1 = hex2byte(p);
    int b0 = hex2byte(p + 2);
    if (b1 < 0 || b0 < 0) return -1;
    *out = (uint16_t)((b1 << 8) | b0);
    return 0;
}

int sos_parse_header(const uint8_t *file, uint32_t size, sos_header_t *hdr)
{
    if (size < SOS_HEADER_SIZE)
        return -1;
    if (memcmp(file, SOS_MAGIC, SOS_MAGIC_LEN) != 0)
        return -1;
    if (file[4] != ' ' || file[7] != ' ' || file[12] != ' ')
        return -1;
    if (file[17] != 0x0A)
        return -1;

    int mode = hex2byte(file + 5);
    if (mode < 0)
        return -1;
    hdr->file_mode = (uint8_t)mode;

    if (hex4_to_u16(file + 8, &hdr->load_addr) < 0)
        return -1;
    if (hex4_to_u16(file + 13, &hdr->exec_addr) < 0)
        return -1;

    return 0;
}

/* ── Everything below is kernel-only (no host-test support yet) ────────── */

#ifdef PPAP_KERNEL

#include "kernel/fd/fd.h"
#include "kernel/signal/signal.h"
#include "kernel/syscall/syscall.h"
#include "kernel/vfs/vfs.h"
#include "common/poll.h"

static void sos_trace_before(uint32_t abi, uint32_t nr, z80_state_t *cpu)
{
    (void)trace_before_subsys(abi, nr,
                              z80_af(cpu), z80_bc(cpu), z80_de(cpu), z80_hl(cpu),
                              cpu->sp, cpu->pc);
}

static void sos_trace_after(uint32_t abi, uint32_t nr, z80_state_t *cpu)
{
    trace_after_subsys(abi, nr,
                       z80_af(cpu), z80_bc(cpu), z80_de(cpu), z80_hl(cpu),
                       cpu->sp, cpu->pc, (int32_t)cpu->a);
}

static void sos_raw_putchar(uint8_t ch)
{
    sys_write(1, (const char *)&ch, 1);
}

static void sos_putstr(const char *s, int len)
{
    sys_write(1, s, (size_t)len);
}

static uint8_t sos_getchar(void)
{
    uint8_t ch = 0;
    for (;;) {
        long rc = sys_read(0, (char *)&ch, 1);
        if (rc > 0)
            return ch;
        signal_check_kernel();
    }
}

static int sos_char_ready(void)
{
    struct pollfd pfd = { .fd = 0, .events = POLLIN, .revents = 0 };
    sys_poll(&pfd, 1, 0);
    return (pfd.revents & POLLIN) ? 1 : 0;
}

static int sos_file_open(const char *path, int flags)
{
    return (int)sys_open(path, (long)flags, 0644);
}

static int sos_file_close(int fd)
{
    return (int)sys_close((long)fd);
}

static int sos_file_read(int fd, void *buf, int count)
{
    return (int)sys_read((long)fd, (char *)buf, (size_t)count);
}

static int sos_file_write(int fd, const void *buf, int count)
{
    return (int)sys_write((long)fd, (const char *)buf, (size_t)count);
}

static int sos_file_delete(const char *path)
{
    return (int)sys_unlink(path);
}

static int sos_file_rename(const char *oldpath, const char *newpath)
{
    return (int)sys_rename(oldpath, newpath);
}

/* ── Helper: read 16-bit LE from Z80 memory ───────────────────────────── */

static uint16_t z80_rd16(z80_state_t *cpu, uint16_t addr)
{
    return (uint16_t)(cpu->memory[addr] | (cpu->memory[addr + 1] << 8));
}

static void z80_wr16(z80_state_t *cpu, uint16_t addr, uint16_t val)
{
    cpu->memory[addr]     = val & 0xFF;
    cpu->memory[addr + 1] = (val >> 8) & 0xFF;
}

/* ── Helper: resolve S-OS filename to PPAP path ──────────────────────── */

static void sos_resolve_path(z80_state_t *cpu, sos_state_t *sos,
                             char *path, int path_size)
{
    /* Read filename from FNAM work area (up to 16 chars) */
    char fnam[17];
    int i;
    for (i = 0; i < 16; i++) {
        char ch = (char)cpu->memory[SOS_FNAM + i];
        if (ch == '\0' || ch == ' ' || ch == '\r' || ch == '\n')
            break;
        fnam[i] = ch;
    }
    fnam[i] = '\0';

    uint8_t session = cpu->memory[SOS_SESSION];
    if (session > 25)
        session = sos->current_session;

    /* Map session to /a/ /b/ etc. */
    char drive = 'a' + session;
    int pos = 0;

    path[pos++] = '/';
    path[pos++] = drive;
    path[pos++] = '/';

    for (int j = 0; fnam[j] && pos < path_size - 1; j++)
        path[pos++] = fnam[j];
    path[pos] = '\0';
}

/* ── S-OS API functions ────────────────────────────────────────────────── */

/* Console output functions */

static int sos_fn_cold(z80_state_t *cpu, sos_state_t *sos)
{
    (void)cpu; (void)sos;
    return ECPU_TRAP_EXIT;
}

static int sos_fn_hot(z80_state_t *cpu, sos_state_t *sos)
{
    (void)cpu; (void)sos;
    return ECPU_TRAP_EXIT;
}

static int sos_fn_ver(z80_state_t *cpu, sos_state_t *sos)
{
    (void)cpu; (void)sos;
    sos_putstr("S-OS SWORD on PPAP\r\n", 20);
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_print(z80_state_t *cpu, sos_state_t *sos)
{
    (void)sos;
    sos_raw_putchar(cpu->a);
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_msg(z80_state_t *cpu, sos_state_t *sos)
{
    (void)sos;
    uint16_t addr = z80_de(cpu);
    while (addr < cpu->mem_size && cpu->memory[addr] != '\0') {
        sos_raw_putchar(cpu->memory[addr]);
        addr++;
    }
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_msx(z80_state_t *cpu, sos_state_t *sos)
{
    /* Printer output — redirect to stdout */
    (void)sos;
    sos_raw_putchar(cpu->a);
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_tab(z80_state_t *cpu, sos_state_t *sos)
{
    (void)sos;
    /* Move cursor to column specified in A (ANSI escape) */
    char buf[16];
    unsigned col = cpu->a + 1;  /* ANSI is 1-based */
    int len = 0;
    buf[len++] = '\033';
    buf[len++] = '[';
    if (col >= 100) buf[len++] = '0' + col / 100;
    if (col >= 10) buf[len++] = '0' + (col / 10) % 10;
    buf[len++] = '0' + col % 10;
    buf[len++] = 'G';
    sos_putstr(buf, len);
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_cr(z80_state_t *cpu, sos_state_t *sos)
{
    (void)cpu; (void)sos;
    sos_raw_putchar('\r');
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_lf(z80_state_t *cpu, sos_state_t *sos)
{
    (void)cpu; (void)sos;
    sos_raw_putchar('\n');
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_bell(z80_state_t *cpu, sos_state_t *sos)
{
    (void)cpu; (void)sos;
    sos_raw_putchar('\a');
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_prthx(z80_state_t *cpu, sos_state_t *sos)
{
    (void)sos;
    static const char hex[] = "0123456789ABCDEF";
    char buf[2];
    buf[0] = hex[(cpu->a >> 4) & 0x0F];
    buf[1] = hex[cpu->a & 0x0F];
    sos_putstr(buf, 2);
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_prthl(z80_state_t *cpu, sos_state_t *sos)
{
    (void)sos;
    static const char hex[] = "0123456789ABCDEF";
    uint16_t hl = z80_hl(cpu);
    char buf[4];
    buf[0] = hex[(hl >> 12) & 0x0F];
    buf[1] = hex[(hl >> 8)  & 0x0F];
    buf[2] = hex[(hl >> 4)  & 0x0F];
    buf[3] = hex[hl & 0x0F];
    sos_putstr(buf, 4);
    return ECPU_TRAP_HANDLED;
}

/* Console input functions */

static int sos_fn_getl(z80_state_t *cpu, sos_state_t *sos)
{
    (void)sos;
    uint16_t buf_addr = z80_de(cpu);
    uint8_t max = cpu->b;
    if (max == 0) max = 255;

    int pos = 0;
    for (;;) {
        uint8_t ch = sos_getchar();
        if (ch == '\r' || ch == '\n') {
            sos_raw_putchar('\r');
            sos_raw_putchar('\n');
            break;
        }
        if ((ch == 0x08 || ch == 0x7F) && pos > 0) {
            pos--;
            sos_raw_putchar(0x08);
            sos_raw_putchar(' ');
            sos_raw_putchar(0x08);
            continue;
        }
        if (pos < max) {
            cpu->memory[buf_addr + pos] = ch;
            pos++;
            sos_raw_putchar(ch);
        }
    }
    cpu->memory[buf_addr + pos] = '\0';
    cpu->b = (uint8_t)pos;
    cpu->f &= ~FLAG_C;
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_getky(z80_state_t *cpu, sos_state_t *sos)
{
    (void)sos;
    cpu->a = sos_getchar();
    cpu->f &= ~FLAG_C;
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_brkey(z80_state_t *cpu, sos_state_t *sos)
{
    (void)sos;
    /* Check if break (Ctrl-C) is pending — approximate with char_ready */
    if (sos_char_ready()) {
        cpu->f |= FLAG_C;  /* carry = break detected */
    } else {
        cpu->f &= ~FLAG_C;
    }
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_inkey(z80_state_t *cpu, sos_state_t *sos)
{
    (void)sos;
    if (sos_char_ready()) {
        cpu->a = sos_getchar();
    } else {
        cpu->a = 0;
    }
    cpu->f &= ~FLAG_C;
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_pause(z80_state_t *cpu, sos_state_t *sos)
{
    (void)sos;
    (void)sos_getchar();  /* wait for any key, discard */
    cpu->f &= ~FLAG_C;
    return ECPU_TRAP_HANDLED;
}

/* Numeric conversion functions */

static int sos_fn_asc1b(z80_state_t *cpu, sos_state_t *sos)
{
    (void)sos;
    /* ASCII digit char in A → binary value in A */
    uint8_t ch = cpu->a;
    if (ch >= '0' && ch <= '9') {
        cpu->a = ch - '0';
        cpu->f &= ~FLAG_C;
    } else {
        cpu->f |= FLAG_C;
    }
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_hex1b(z80_state_t *cpu, sos_state_t *sos)
{
    (void)sos;
    int val = hex_digit(cpu->a);
    if (val >= 0) {
        cpu->a = (uint8_t)val;
        cpu->f &= ~FLAG_C;
    } else {
        cpu->f |= FLAG_C;
    }
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_hex2b(z80_state_t *cpu, sos_state_t *sos)
{
    (void)sos;
    /* HL points to 2 hex chars in memory */
    uint16_t addr = z80_hl(cpu);
    int val = hex2byte(&cpu->memory[addr]);
    if (val >= 0) {
        cpu->a = (uint8_t)val;
        cpu->f &= ~FLAG_C;
    } else {
        cpu->f |= FLAG_C;
    }
    return ECPU_TRAP_HANDLED;
}

/* I/O port stubs */

static int sos_fn_flget(z80_state_t *cpu, sos_state_t *sos)
{
    (void)sos;
    cpu->f |= FLAG_C;  /* error: no tape */
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_rdvsw(z80_state_t *cpu, sos_state_t *sos)
{
    (void)sos;
    cpu->a = 0;
    cpu->f &= ~FLAG_C;
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_sdvsw(z80_state_t *cpu, sos_state_t *sos)
{
    (void)cpu; (void)sos;
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_inp(z80_state_t *cpu, sos_state_t *sos)
{
    (void)sos;
    cpu->a = 0;
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_out(z80_state_t *cpu, sos_state_t *sos)
{
    (void)cpu; (void)sos;
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_widch(z80_state_t *cpu, sos_state_t *sos)
{
    (void)sos;
    /* Store screen width in work area */
    z80_wr16(cpu, SOS_MAXCOL, (uint16_t)cpu->a);
    cpu->f &= ~FLAG_C;
    return ECPU_TRAP_HANDLED;
}

/* File operations */

static int sos_fn_file(z80_state_t *cpu, sos_state_t *sos)
{
    /* Directory listing — stub: return with carry set (not implemented) */
    (void)cpu; (void)sos;
    cpu->f |= FLAG_C;
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_fsave(z80_state_t *cpu, sos_state_t *sos)
{
    char path[64];
    sos_resolve_path(cpu, sos, path, sizeof(path));

    uint16_t dtadr = z80_rd16(cpu, SOS_DTADR);
    uint16_t edadr = z80_rd16(cpu, SOS_EDADR);

    if (edadr < dtadr || dtadr >= cpu->mem_size) {
        cpu->f |= FLAG_C;
        return ECPU_TRAP_HANDLED;
    }

    uint32_t len = (uint32_t)(edadr - dtadr + 1);
    if (dtadr + len > cpu->mem_size)
        len = cpu->mem_size - dtadr;

    int fd = sos_file_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        cpu->f |= FLAG_C;
        return ECPU_TRAP_HANDLED;
    }

    int written = sos_file_write(fd, &cpu->memory[dtadr], (int)len);
    sos_file_close(fd);

    if (written < 0 || (uint32_t)written != len) {
        cpu->f |= FLAG_C;
    } else {
        cpu->f &= ~FLAG_C;
    }
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_fload(z80_state_t *cpu, sos_state_t *sos)
{
    char path[64];
    sos_resolve_path(cpu, sos, path, sizeof(path));

    uint16_t dtadr = z80_rd16(cpu, SOS_DTADR);

    if (dtadr >= cpu->mem_size) {
        cpu->f |= FLAG_C;
        return ECPU_TRAP_HANDLED;
    }

    int fd = sos_file_open(path, O_RDONLY);
    if (fd < 0) {
        cpu->f |= FLAG_C;
        return ECPU_TRAP_HANDLED;
    }

    uint32_t max_len = cpu->mem_size - dtadr;
    int nread = sos_file_read(fd, &cpu->memory[dtadr], (int)max_len);
    sos_file_close(fd);

    if (nread < 0) {
        cpu->f |= FLAG_C;
    } else {
        /* Update EDADR to reflect actual end of loaded data */
        z80_wr16(cpu, SOS_EDADR, (uint16_t)(dtadr + nread - 1));
        cpu->f &= ~FLAG_C;
    }
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_fvrfy(z80_state_t *cpu, sos_state_t *sos)
{
    char path[64];
    sos_resolve_path(cpu, sos, path, sizeof(path));

    uint16_t dtadr = z80_rd16(cpu, SOS_DTADR);
    uint16_t edadr = z80_rd16(cpu, SOS_EDADR);

    if (edadr < dtadr || dtadr >= cpu->mem_size) {
        cpu->f |= FLAG_C;
        return ECPU_TRAP_HANDLED;
    }

    int fd = sos_file_open(path, O_RDONLY);
    if (fd < 0) {
        cpu->f |= FLAG_C;
        return ECPU_TRAP_HANDLED;
    }

    uint32_t len = (uint32_t)(edadr - dtadr + 1);
    uint8_t buf[128];
    uint32_t offset = 0;
    int match = 1;

    while (offset < len) {
        int chunk = (int)(len - offset);
        if (chunk > (int)sizeof(buf)) chunk = (int)sizeof(buf);
        int nread = sos_file_read(fd, buf, chunk);
        if (nread <= 0) {
            match = 0;
            break;
        }
        if (memcmp(&cpu->memory[dtadr + offset], buf, (size_t)nread) != 0) {
            match = 0;
            break;
        }
        offset += (uint32_t)nread;
    }
    sos_file_close(fd);

    if (match)
        cpu->f &= ~FLAG_C;
    else
        cpu->f |= FLAG_C;
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_fkill(z80_state_t *cpu, sos_state_t *sos)
{
    char path[64];
    sos_resolve_path(cpu, sos, path, sizeof(path));

    if (sos_file_delete(path) < 0)
        cpu->f |= FLAG_C;
    else
        cpu->f &= ~FLAG_C;
    return ECPU_TRAP_HANDLED;
}

static int sos_fn_fren(z80_state_t *cpu, sos_state_t *sos)
{
    /* Old name in FNAM work area, new name pointed by DE */
    char oldpath[64];
    sos_resolve_path(cpu, sos, oldpath, sizeof(oldpath));

    /* Read new filename from DE */
    uint16_t de = z80_de(cpu);
    char newname[17];
    int i;
    for (i = 0; i < 16 && (uint32_t)(de + i) < cpu->mem_size; i++) {
        char ch = (char)cpu->memory[de + i];
        if (ch == '\0' || ch == ' ' || ch == '\r' || ch == '\n')
            break;
        newname[i] = ch;
    }
    newname[i] = '\0';

    /* Build new path with same session prefix */
    char newpath[64];
    uint8_t session = cpu->memory[SOS_SESSION];
    if (session > 25)
        session = sos->current_session;
    char drive = 'a' + session;
    int pos = 0;
    newpath[pos++] = '/';
    newpath[pos++] = drive;
    newpath[pos++] = '/';
    for (int j = 0; newname[j] && pos < 63; j++)
        newpath[pos++] = newname[j];
    newpath[pos] = '\0';

    if (sos_file_rename(oldpath, newpath) < 0)
        cpu->f |= FLAG_C;
    else
        cpu->f &= ~FLAG_C;
    return ECPU_TRAP_HANDLED;
}

/* ── S-OS API dispatch ─────────────────────────────────────────────────── */

static int sos_dispatch(z80_state_t *cpu, sos_state_t *sos)
{
    uint8_t fn = cpu->a;

    sos_trace_before(PPAP_TRACE_ABI_SOS, fn, cpu);

    int rc;
    switch (fn) {
    case SOS_FN_COLD:   rc = sos_fn_cold(cpu, sos);   break;
    case SOS_FN_HOT:    rc = sos_fn_hot(cpu, sos);    break;
    case SOS_FN_VER:    rc = sos_fn_ver(cpu, sos);    break;
    case SOS_FN_PRINT:  rc = sos_fn_print(cpu, sos);  break;
    case SOS_FN_MSG:    rc = sos_fn_msg(cpu, sos);    break;
    case SOS_FN_MSX:    rc = sos_fn_msx(cpu, sos);    break;
    case SOS_FN_TAB:    rc = sos_fn_tab(cpu, sos);    break;
    case SOS_FN_CR:     rc = sos_fn_cr(cpu, sos);     break;
    case SOS_FN_LF:     rc = sos_fn_lf(cpu, sos);     break;
    case SOS_FN_GETL:   rc = sos_fn_getl(cpu, sos);   break;
    case SOS_FN_GETKY:  rc = sos_fn_getky(cpu, sos);  break;
    case SOS_FN_BRKEY:  rc = sos_fn_brkey(cpu, sos);  break;
    case SOS_FN_INKEY:  rc = sos_fn_inkey(cpu, sos);  break;
    case SOS_FN_PAUSE:  rc = sos_fn_pause(cpu, sos);  break;
    case SOS_FN_BELL:   rc = sos_fn_bell(cpu, sos);   break;
    case SOS_FN_PRTHX:  rc = sos_fn_prthx(cpu, sos);  break;
    case SOS_FN_PRTHL:  rc = sos_fn_prthl(cpu, sos);  break;
    case SOS_FN_ASC1B:  rc = sos_fn_asc1b(cpu, sos);  break;
    case SOS_FN_HEX1B:  rc = sos_fn_hex1b(cpu, sos);  break;
    case SOS_FN_HEX2B:  rc = sos_fn_hex2b(cpu, sos);  break;
    case SOS_FN_FLGET:  rc = sos_fn_flget(cpu, sos);  break;
    case SOS_FN_RDVSW:  rc = sos_fn_rdvsw(cpu, sos);  break;
    case SOS_FN_SDVSW:  rc = sos_fn_sdvsw(cpu, sos);  break;
    case SOS_FN_INP:    rc = sos_fn_inp(cpu, sos);    break;
    case SOS_FN_OUT:    rc = sos_fn_out(cpu, sos);    break;
    case SOS_FN_WIDCH:  rc = sos_fn_widch(cpu, sos);  break;
    case SOS_FN_FILE:   rc = sos_fn_file(cpu, sos);   break;
    case SOS_FN_FSAVE:  rc = sos_fn_fsave(cpu, sos);  break;
    case SOS_FN_FLOAD:  rc = sos_fn_fload(cpu, sos);  break;
    case SOS_FN_FVRFY:  rc = sos_fn_fvrfy(cpu, sos);  break;
    case SOS_FN_FKILL:  rc = sos_fn_fkill(cpu, sos);  break;
    case SOS_FN_FREN:   rc = sos_fn_fren(cpu, sos);   break;
    default:
        cpu->f |= FLAG_C;
        rc = ECPU_TRAP_HANDLED;
        break;
    }

    sos_trace_after(PPAP_TRACE_ABI_SOS, fn, cpu);
    return rc;
}

/* ── Trap handler ──────────────────────────────────────────────────────── */

int sos_trap_handler(ecpu_state_t *state, int trap_type,
                     uint32_t param, void *ctx)
{
    z80_state_t *cpu = (z80_state_t *)state;
    sos_state_t *sos = (sos_state_t *)ctx;

    if (trap_type == ECPU_TRAP_CALL) {
        /* RST 18h is trapped as CALL to 0x0018, or CALL to SOS_ENTRY */
        if (param == SOS_RST18_ADDR || param == SOS_ENTRY)
            return sos_dispatch(cpu, sos);
        /* Return to 0x0000 = cold start = exit */
        if (param == 0x0000 || param == SOS_COLD_ENTRY)
            return ECPU_TRAP_EXIT;
    }

    if (trap_type == ECPU_TRAP_HALT)
        return ECPU_TRAP_EXIT;

    return ECPU_TRAP_UNHANDLED;
}

/* ── Kernel-mode process entry and subsys ops ─────────────────────────── */

#include "kernel/proc/proc.h"
#include "kernel/proc/sched.h"
#include "subsys.h"

void sos_run_process(void)
{
    pcb_t *p = current;

    /* Recover per-process state: { z80_state_t, sos_state_t } */
    z80_state_t *z80 = (z80_state_t *)p->subsys_data;
    sos_state_t *sos = (sos_state_t *)((char *)p->subsys_data
                                        + sizeof(z80_state_t));

    /* Switch TTY to raw mode (same pattern as CP/M) */
    {
        struct { uint32_t iflag, oflag, cflag, lflag; uint8_t line; uint8_t cc[19]; } t;
        sys_ioctl(0, 0x5401/*TCGETS*/, (long)&t);
        sos->saved_termios[0] = t.iflag;
        sos->saved_termios[1] = t.oflag;
        sos->saved_termios[2] = t.cflag;
        sos->saved_termios[3] = t.lflag;
        sos->saved_termios[4] = t.line;
        __builtin_memcpy(sos->saved_termios_cc, t.cc, 19);
        sos->termios_saved = 1;
        t.lflag &= ~0x000Au;   /* clear ICANON | ECHO */
        t.iflag &= ~0x0100u;   /* clear ICRNL */
        sys_ioctl(0, 0x5402/*TCSETS*/, (long)&t);
    }

    for (;;) {
        int do_step_stop = p->trace_step_pending != 0;
        int run_with_step = do_step_stop ||
                            (p->tracer_pid != 0 && trace_has_swbp());

        if (run_with_step) {
            p->trace_step_pending = 0;

            if (trace_maybe_stop_at_swbp(PPAP_TRACE_ABI_SOS, z80->pc)) {
                if (p->state == PROC_TRACED_STOP)
                    sched_yield();
                continue;
            }

            if (ecpu_z80_ops.step &&
                ecpu_z80_ops.step((ecpu_state_t *)z80) != 0)
                break;
            if (do_step_stop && p->tracer_pid != 0 && p->state == PROC_RUNNABLE)
                trace_debug_stop(PPAP_TRACE_ABI_SOS, z80->pc,
                                 PPAP_DEBUG_STOP_STEP);
            if (p->state == PROC_TRACED_STOP)
                sched_yield();
            continue;
        }

        ecpu_z80_ops.run((ecpu_state_t *)z80);
        break;
    }

    sys_exit(0);
}

static void sos_on_exit(struct pcb *p)
{
    if (!p->subsys_data)
        return;

    sos_state_t *sos = (sos_state_t *)((char *)p->subsys_data
                                        + sizeof(z80_state_t));
    if (!sos->termios_saved)
        return;

    struct { uint32_t iflag, oflag, cflag, lflag; uint8_t line; uint8_t cc[19]; } t;
    t.iflag = sos->saved_termios[0];
    t.oflag = sos->saved_termios[1];
    t.cflag = sos->saved_termios[2];
    t.lflag = sos->saved_termios[3];
    t.line  = (uint8_t)sos->saved_termios[4];
    __builtin_memcpy(t.cc, sos->saved_termios_cc, 19);
    sys_ioctl(0, 0x5402/*TCSETS*/, (long)&t);
    sos->termios_saved = 0;
}

const subsys_ops_t sos_subsys_ops = {
    .on_crash  = (void *)0,
    .on_signal = (void *)0,
    .on_init   = (void *)0,
    .on_exit   = sos_on_exit,
};

#endif /* PPAP_KERNEL */
