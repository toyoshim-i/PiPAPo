/*
 * sos_bridge.h — S-OS "SWORD" subsystem state and constants
 *
 * Defines the per-process S-OS state (sos_state_t), memory map addresses,
 * and the public API used by the loader and trap handler.
 *
 * See docs/proposals/sos_subsystem.md for the design.
 */

#ifndef PPAP_SUBSYS_SOS_BRIDGE_H
#define PPAP_SUBSYS_SOS_BRIDGE_H

#include <stdint.h>
#include "config.h"
#include "kernel/ecpu/ecpu.h"
#include "kernel/ecpu/ecpu_z80.h"
#include "subsys.h"

/* ── S-OS _SOS header format ───────────────────────────────────────────── */
#define SOS_MAGIC         "_SOS"
#define SOS_MAGIC_LEN     4
#define SOS_HEADER_SIZE   18       /* "_SOS 01 3000 3000\n" */
#define SOS_MODE_BINARY   0x01
#define SOS_MODE_ASCII    0x04

/* ── S-OS memory map constants ─────────────────────────────────────────── */
#define SOS_RST18_ADDR    0x0018   /* RST 18h vector address               */
#define SOS_HOOK_BASE     0x0100   /* Subroutine hooks (0100h–017Fh)       */
#define SOS_INBUF_ADDR    0x0180   /* Input buffer (0180h–019Fh)           */
#define SOS_CURSOR_ADDR   0x01A0   /* Cursor position (01A0h–01A1h)        */
#define SOS_KBUF_ADDR     0x0200   /* Keyboard input buffer                */
#define SOS_FAT_ADDR      0x0300   /* FAT buffer                           */
#define SOS_STACK_ADDR    0x0600   /* Stack area (0600h–07FFh)             */
#define SOS_STACK_TOP     0x0800   /* Stack grows downward from here       */
#define SOS_WORK_BASE     0x1F80   /* Work area base                       */
#define SOS_FNAM          0x1F80   /* Filename buffer (16 bytes)           */
#define SOS_FTYPE         0x1F90   /* File type byte                       */
#define SOS_DTADR         0x1F91   /* Data (load) address, 2 bytes LE      */
#define SOS_EDADR         0x1F93   /* End address, 2 bytes LE              */
#define SOS_EXADR         0x1F95   /* Execution address, 2 bytes LE        */
#define SOS_SESSION        0x1F97  /* Current file session (drive)         */
#define SOS_DSESSION       0x1F98  /* Default session                      */
#define SOS_MAXLIN        0x1F99   /* Screen max lines (2 bytes)           */
#define SOS_MAXCOL        0x1F9B   /* Screen max columns (2 bytes)         */
#define SOS_ENTRY         0x1FFB   /* API alternate entry: JP handler      */
#define SOS_COLD_ENTRY    0x1FFE   /* Cold start entry                     */
#define SOS_DEFAULT_LOAD  0x2000   /* Default load address                 */

/* ── S-OS API function numbers (A register) ────────────────────────────── */
#define SOS_FN_COLD       0x00     /* #COLD  — cold start (exit)           */
#define SOS_FN_HOT        0x01     /* #HOT   — warm start (exit)           */
#define SOS_FN_VER        0x02     /* #VER   — print version               */
#define SOS_FN_PRINT      0x03     /* #PRINT — print char in A             */
#define SOS_FN_MSG        0x04     /* #MSG   — print NUL-terminated at DE  */
#define SOS_FN_MSX        0x05     /* #MSX   — printer output              */
#define SOS_FN_TAB        0x06     /* #TAB   — move to column              */
#define SOS_FN_CR         0x07     /* #CR    — carriage return             */
#define SOS_FN_LF         0x08     /* #LF    — line feed                   */
#define SOS_FN_GETL       0x09     /* #GETL  — line input                  */
#define SOS_FN_GETKY      0x0A     /* #GETKY — wait for key                */
#define SOS_FN_BRKEY      0x0B     /* #BRKEY — check break key             */
#define SOS_FN_INKEY      0x0C     /* #INKEY — non-blocking key check      */
#define SOS_FN_PAUSE      0x0D     /* #PAUSE — wait for any key            */
#define SOS_FN_BELL       0x0E     /* #BELL  — sound bell                  */
#define SOS_FN_PRTHX      0x0F     /* #PRTHX — print A as 2-digit hex     */
#define SOS_FN_PRTHL      0x10     /* #PRTHL — print HL as 4-digit hex    */
#define SOS_FN_ASC1B      0x11     /* #ASC1B — ASCII to 1-byte value      */
#define SOS_FN_HEX1B      0x12     /* #HEX1B — hex char to byte           */
#define SOS_FN_HEX2B      0x13     /* #HEX2B — 2 hex chars to byte        */
#define SOS_FN_FLGET      0x14     /* #FLGET — get char from tape (stub)  */
#define SOS_FN_RDVSW      0x15     /* #RDVSW — read DIP switch (stub)     */
#define SOS_FN_SDVSW      0x16     /* #SDVSW — set DIP switch (stub)      */
#define SOS_FN_INP        0x17     /* #INP   — input from I/O port (stub) */
#define SOS_FN_OUT        0x18     /* #OUT   — output to I/O port (stub)  */
#define SOS_FN_WIDCH      0x19     /* #WIDCH — set screen width            */
#define SOS_FN_FILE       0x1A     /* #FILE  — directory listing           */
#define SOS_FN_FSAVE      0x1B     /* #FSAVE — save memory to file         */
#define SOS_FN_FLOAD      0x1C     /* #FLOAD — load file to memory         */
#define SOS_FN_FVRFY      0x1D     /* #FVRFY — verify file against memory  */
#define SOS_FN_FKILL      0x1E     /* #FKILL — delete file                 */
#define SOS_FN_FREN       0x1F     /* #FREN  — rename file                 */

/* ── Per-process S-OS state ────────────────────────────────────────────── */
typedef struct sos_state {
    uint8_t  current_session;  /* Current file session/drive (0=A, 1=B...)  */
    uint8_t  default_session;  /* Default session                           */

    /* Saved terminal state (restored on exit) */
    uint32_t saved_termios[5];
    uint8_t  saved_termios_cc[19];
    uint8_t  termios_saved;
} sos_state_t;

/* ── Parsed _SOS header ────────────────────────────────────────────────── */
typedef struct sos_header {
    uint16_t load_addr;   /* from hex ASCII at +8  */
    uint16_t exec_addr;   /* from hex ASCII at +13 */
    uint8_t  file_mode;   /* 0x01=binary, 0x04=ASCII */
} sos_header_t;

/* Parse a _SOS header.  Returns 0 on success, -1 on invalid header. */
int sos_parse_header(const uint8_t *file, uint32_t size, sos_header_t *hdr);

/* ── Trap handler (personality layer) ──────────────────────────────────── */

int sos_trap_handler(ecpu_state_t *cpu, int trap_type,
                     uint32_t param, void *ctx);

/* Kernel-mode entry point for S-OS processes */
void sos_run_process(void);

/* Subsystem ops — registered into subsys_ops_table[] */
extern const subsys_ops_t sos_subsys_ops;

#endif /* PPAP_SUBSYS_SOS_BRIDGE_H */
