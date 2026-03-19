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
#include "kernel/cpu/cpu.h"
#include "kernel/cpu/ecpu_z80.h"
#include "subsys.h"

/* ── S-OS _SOS header format ───────────────────────────────────────────── */
#define SOS_MAGIC         "_SOS"
#define SOS_MAGIC_LEN     4
#define SOS_HEADER_SIZE   18       /* "_SOS 01 3000 3000\n" */
#define SOS_MODE_BINARY   0x01
#define SOS_MODE_ASCII    0x04

/* ── S-OS memory map constants ─────────────────────────────────────────── */
#define SOS_RST18_ADDR    0x0018   /* RST 18h vector address               */
#define SOS_STUB_BASE     0x0100   /* Internal RST stub area (fn×2 bytes)  */
#define SOS_STACK_TOP     0x0800   /* Stack grows downward from here       */

/* Work area (0x1F40–0x1F7F) — matches the SWORD specification */
#define SOS_FNAM          0x1F40   /* Filename buffer (16 bytes, PPAP ext)  */
#define SOS_MXLIN         0x1F5B   /* Screen height (1 byte)               */
#define SOS_WIDTH         0x1F5C   /* Screen width (1 byte)                */
#define SOS_DSK           0x1F5D   /* Current device name (1 byte)         */
#define SOS_FATPS         0x1F5E   /* FAT sector count (2 bytes)           */
#define SOS_DIRPS         0x1F60   /* Directory record count (2 bytes)     */
#define SOS_FATBF         0x1F62   /* FAT buffer address (2 bytes)         */
#define SOS_DTBUF         0x1F64   /* Data buffer address (2 bytes)        */
#define SOS_MXTRK         0x1F66   /* Max tracks (1 byte)                  */
#define SOS_DIRNO         0x1F67   /* Directory entry count (1 byte)       */
#define SOS_WKSIZ         0x1F68   /* Work area size (2 bytes)             */
#define SOS_MEMAX         0x1F6A   /* User RAM limit (2 bytes)             */
#define SOS_STKAD         0x1F6C   /* Stack pointer (2 bytes)              */
#define SOS_EXADR         0x1F6E   /* Execution address (2 bytes LE)       */
#define SOS_DTADR         0x1F70   /* Data (load) address (2 bytes LE)     */
#define SOS_SIZE          0x1F72   /* File size (2 bytes LE)               */
#define SOS_IBFAD         0x1F74   /* File I/O control block addr (2 bytes)*/
#define SOS_KBFAD         0x1F76   /* Keyboard buffer address (2 bytes)    */
#define SOS_XYADR         0x1F78   /* Cursor coordinate address (2 bytes)  */
#define SOS_PRCNT         0x1F7A   /* Print position counter addr (2 bytes)*/
#define SOS_LPSW          0x1F7C   /* Printer flag (1 byte)                */
#define SOS_DVSW          0x1F7D   /* Device: 0=FDD, 1=QD (1 byte)        */
#define SOS_USR           0x1F7E   /* User startup address (2 bytes)       */

/* ── Monitor subroutine entry points ──────────────────────────────────── */
/*
 * The S-OS monitor jump table lives at 0x1F80–0x1FFF.  Each entry is
 * 3 bytes (a JP instruction).  Entries count *downward* from 0x1FFD:
 *   addr = 0x1FFD − fn_index × 3
 */
#define SOS_MON_BASE      0x1F80   /* Lowest monitor address (#MON=0x1F8E) */
#define SOS_MON_TOP       0x1FFD   /* Highest: #COLD entry                 */
#define SOS_MON_FN(addr)  (((uint32_t)SOS_MON_TOP - (addr)) / 3u)

/* Extended API jump table at 0x2000+ */
#define SOS_EXT_BASE      0x2000
#define SOS_EXT_TOP       0x2036
#define SOS_EXT_FN(addr)  (40u + ((addr) - (uint32_t)SOS_EXT_BASE) / 3u)

#define SOS_COLD_ENTRY    0x1FFD   /* #COLD entry = fn 0                   */
#define SOS_DEFAULT_LOAD  0x2000   /* Default load address                 */

/*
 * S-OS API function indices (used with RST 18h and for monitor dispatch).
 * Standard API: fn = (0x1FFD − addr) / 3  → address = 0x1FFD − fn × 3
 */
#define SOS_FN_COLD       0    /* 0x1FFD  #COLD   — cold start (exit)      */
#define SOS_FN_HOT        1    /* 0x1FFA  #HOT    — warm start (exit)      */
#define SOS_FN_VER        2    /* 0x1FF7  #VER    — print version          */
#define SOS_FN_PRINT      3    /* 0x1FF4  #PRINT  — print char in A        */
#define SOS_FN_PRINTS     4    /* 0x1FF1  #PRINTS — print string at DE     */
#define SOS_FN_LTNL       5    /* 0x1FEE  #LTNL   — line feed (LF)         */
#define SOS_FN_NL         6    /* 0x1FEB  #NL     — newline (CR+LF)        */
#define SOS_FN_MSG        7    /* 0x1FE8  #MSG    — print msg, ends 0Dh    */
#define SOS_FN_MSX        8    /* 0x1FE5  #MSX    — print str, ends 00h    */
#define SOS_FN_MPRINT     9    /* 0x1FE2  #MPRINT — monitor print          */
#define SOS_FN_TAB       10    /* 0x1FDF  #TAB    — move to column         */
#define SOS_FN_LPRINT    11    /* 0x1FDC  #LPRINT — line printer print     */
#define SOS_FN_LPTON     12    /* 0x1FD9  #LPTON  — line printer on        */
#define SOS_FN_LPTOF     13    /* 0x1FD6  #LPTOF  — line printer off       */
#define SOS_FN_GETL      14    /* 0x1FD3  #GETL   — line input             */
#define SOS_FN_GETKY     15    /* 0x1FD0  #GETKY  — scan key (non-block)   */
#define SOS_FN_BRKEY     16    /* 0x1FCD  #BRKEY  — check break (Z flag)   */
#define SOS_FN_INKEY     17    /* 0x1FCA  #INKEY  — wait for key (block)   */
#define SOS_FN_PAUSE     18    /* 0x1FC7  #PAUSE  — wait for any key       */
#define SOS_FN_BELL      19    /* 0x1FC4  #BELL   — sound bell             */
#define SOS_FN_PRTHX     20    /* 0x1FC1  #PRTHX  — print A as hex byte   */
#define SOS_FN_PRTHL     21    /* 0x1FBE  #PRTHL  — print HL as hex word  */
#define SOS_FN_ASC       22    /* 0x1FBB  #ASC    — 4-bit val to ASCII     */
#define SOS_FN_HEX       23    /* 0x1FB8  #HEX    — ASCII hex to 4-bit    */
#define SOS_FN_2HEX      24    /* 0x1FB5  #2HEX   — 2 hex chars to byte   */
#define SOS_FN_HLHEX     25    /* 0x1FB2  #HLHEX  — HL to hex string      */
#define SOS_FN_WOPEN     26    /* 0x1FAF  #WOPEN  — file write open        */
#define SOS_FN_WRD       27    /* 0x1FAC  #WRD    — file write data        */
#define SOS_FN_FCB       28    /* 0x1FA9  #FCB    — file control block     */
#define SOS_FN_RDD       29    /* 0x1FA6  #RDD    — file read data         */
#define SOS_FN_FILE      30    /* 0x1FA3  #FILE   — directory listing      */
#define SOS_FN_FSAME     31    /* 0x1FA0  #FSAME  — filename compare       */
#define SOS_FN_FPRNT     32    /* 0x1F9D  #FPRNT  — file info print       */
#define SOS_FN_POKE      33    /* 0x1F9A  #POKE   — poke memory           */
#define SOS_FN_POKEA     34    /* 0x1F97  #POKEA  — poke memory (addr)    */
#define SOS_FN_PEEK      35    /* 0x1F94  #PEEK   — peek memory           */
#define SOS_FN_PEEKA     36    /* 0x1F91  #PEEKA  — peek memory (addr)    */
#define SOS_FN_MON       37    /* 0x1F8E  #MON    — enter monitor (exit)  */
/* Extended API (0x2000+) */
#define SOS_FN_DRDSB     40    /* 0x2000  #DRDSB  — disk read sector       */
#define SOS_FN_DWTSB     41    /* 0x2003  #DWTSB  — disk write sector      */
#define SOS_FN_DIR       42    /* 0x2006  #DIR    — directory              */
#define SOS_FN_ROPEN     43    /* 0x2009  #ROPEN  — file read open         */
#define SOS_FN_SET       44    /* 0x200C  #SET    — set session            */
#define SOS_FN_RESET     45    /* 0x200F  #RESET  — reset session          */
#define SOS_FN_NAME      46    /* 0x2012  #NAME   — rename file            */
#define SOS_FN_KILL      47    /* 0x2015  #KILL   — delete file            */
#define SOS_FN_CSR       48    /* 0x2018  #CSR    — cursor position        */
#define SOS_FN_SCRN      49    /* 0x201B  #SCRN   — screen read            */
#define SOS_FN_LOC       50    /* 0x201E  #LOC    — locate cursor          */
#define SOS_FN_FLGET     51    /* 0x2021  #FLGET  — tape read (stub)       */
#define SOS_FN_RDVSW     52    /* 0x2024  #RDVSW  — read DIP switch        */
#define SOS_FN_SDVSW     53    /* 0x2027  #SDVSW  — set DIP switch         */
#define SOS_FN_INP       54    /* 0x202A  #INP    — I/O port input         */
#define SOS_FN_OUT       55    /* 0x202D  #OUT    — I/O port output        */
#define SOS_FN_WIDCH     56    /* 0x2030  #WIDCH  — set screen width       */
#define SOS_FN_ERROR     57    /* 0x2033  #ERROR  — error handler          */
#define SOS_FN_BOOT      58    /* 0x2036  #BOOT   — reboot (exit)          */
#define SOS_FN_MAX       59

/* ── Screen buffer limits ─────────────────────────────────────────────── */
#define SOS_SCREEN_MAX_COLS 80
#define SOS_SCREEN_MAX_ROWS 40

/* ── Per-process S-OS state ────────────────────────────────────────────── */
typedef struct sos_state {
    uint8_t  current_session;  /* Current file session/drive (0=A, 1=B...)  */
    uint8_t  default_session;  /* Default session                           */
    int      file_fd;          /* Open file descriptor for #WOPEN/#ROPEN   */

    /* Screen state */
    uint8_t  cursor_x;         /* Current cursor column (0-based)           */
    uint8_t  cursor_y;         /* Current cursor row (0-based)              */
    uint8_t  screen_width;     /* 40 or 80                                  */
    uint8_t  screen_height;    /* typically 25                               */

    /* Unsupported API tracking — bit N set = function N was called.
     * Split into two 32-bit words to avoid 64-bit shifts on m68k. */
    uint32_t unsupported_lo;  /* functions 0–31 */
    uint32_t unsupported_hi;  /* functions 32–58 */

    /* Saved terminal state (restored on exit) */
    uint32_t saved_termios[5];
    uint8_t  saved_termios_cc[19];
    uint8_t  termios_saved;

    /* Screen character buffer for #SCRN */
    uint8_t  screen_buf[SOS_SCREEN_MAX_COLS * SOS_SCREEN_MAX_ROWS];
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

int sos_trap_handler(cpu_state_t *cpu, int trap_type,
                     uint32_t param, void *ctx);

/* Kernel-mode entry point for S-OS processes */
void sos_run_process(void);

/* Subsystem ops — registered into subsys_ops_table[] */
extern const subsys_ops_t sos_subsys_ops;

#endif /* PPAP_SUBSYS_SOS_BRIDGE_H */
