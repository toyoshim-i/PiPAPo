/*
 * uart_x68k.c — Console driver for X68000 via IPL IOCS
 *
 * Implements the uart.h interface using X68000 IOCS calls (TRAP #15).
 * Stage2 preserves the IPL IOCS handler at vector 47 (TRAP #15), so all
 * IOCS function codes work transparently from supervisor mode.
 *
 * IMPORTANT: All IOCS calls are wrapped with ipl7_save/ipl7_restore to
 * prevent the MFP Timer-C ISR from preempting mid-call.  IOCS functions
 * are not reentrant — they use a shared work area in low RAM.
 *
 * VT100 escape sequence converter:
 *   X68000 IOCS _B_PUTC has its own escape sequence parser that is NOT
 *   VT100-compatible.  Sending VT100 CSI sequences directly causes the
 *   IOCS parser to enter a bad state → address error crash.  This driver
 *   intercepts ESC sequences, parses them as VT100, and converts supported
 *   sequences into equivalent IOCS calls.  Unsupported sequences are
 *   silently dropped.
 *
 * IOCS functions used:
 *   _B_PUTC    (d0=0x20, d1.w=char)  Output one character to TVRAM console
 *   _B_GETC    (d0=0x21)             Input one character (blocking)
 *   _B_KEYSNS  (d0=0x1C)             Keyboard sense (non-blocking)
 *   _B_LOCATE  (d0=0x17, d1.w=x, d2.w=y)  Set cursor position
 *   _B_CLR_ST  (d0=0x22)             Clear entire screen
 *   _B_CLR_ED  (d0=0x23)             Clear from cursor to end of screen
 *   _B_CLR_AL  (d0=0x19)             Clear current line from cursor
 *   _OUT232C   (d0=0x35, d1.b=char)  Output one character to RS-232C serial
 */

#include "drivers/uart.h"
#include <stdint.h>

/* ── IRQ guard: raise IPL around IOCS calls ────────────────────────────── *
 *
 * IOCS functions are NOT reentrant — they share work areas in low RAM.
 * We must prevent the scheduler (MFP Timer-C, IPL level 2) from preempting
 * mid-call.  However, IOCS itself depends on hardware interrupts:
 *   - VSYNC (level 4): _B_PUTC waits for VSYNC during scroll
 *   - SCC (level 5): serial RX buffering for _ISNS232C/_INP232C
 *   - Keyboard (level 6 via MFP): USART RX for _B_KEYSNS/_B_GETC
 *
 * Solution: raise IPL to 3 — blocks the timer (level 2) but allows all
 * hardware interrupts that IOCS depends on (levels 4-7).
 */

static inline uint16_t iocs_ipl_save(void)
{
    uint16_t sr;
    asm volatile("move.w %%sr,%0\n\t"
                 "move.w %0,%%d0\n\t"
                 "andi.w #0xF8FF,%%d0\n\t"
                 "ori.w  #0x0300,%%d0\n\t"
                 "move.w %%d0,%%sr"
                 : "=d"(sr) : : "d0", "memory");
    return sr;
}

static inline void iocs_ipl_restore(uint16_t sr)
{
    asm volatile("move.w %0,%%sr" : : "d"(sr) : "memory");
}

/* ── IOCS call wrappers ────────────────────────────────────────────────── */

static inline void iocs_b_clr_st(void)
{
    register int32_t d0 asm("d0") = 0x22;
    asm volatile("trap #15"
                 : "+r"(d0)
                 : : "d1", "d2", "a0", "a1", "memory");
}

static inline void iocs_b_clr_ed(void)
{
    register int32_t d0 asm("d0") = 0x23;
    asm volatile("trap #15"
                 : "+r"(d0)
                 : : "d1", "d2", "a0", "a1", "memory");
}

static inline void iocs_b_clr_al(void)
{
    register int32_t d0 asm("d0") = 0x19;
    asm volatile("trap #15"
                 : "+r"(d0)
                 : : "d1", "d2", "a0", "a1", "memory");
}

static inline void iocs_b_putc(char c)
{
    register int32_t d0 asm("d0") = 0x20;
    register int32_t d1 asm("d1") = (unsigned char)c;
    asm volatile("trap #15"
                 : "+r"(d0)
                 : "r"(d1)
                 : "d2", "a0", "a1", "memory");
}

static inline void iocs_b_locate(int x, int y)
{
    register int32_t d0 asm("d0") = 0x17;
    register int32_t d1 asm("d1") = x;
    register int32_t d2 asm("d2") = y;
    asm volatile("trap #15"
                 : "+r"(d0)
                 : "r"(d1), "r"(d2)
                 : "a0", "a1", "memory");
}

static inline void iocs_out232c(char c)
{
    register int32_t d0 asm("d0") = 0x35;
    register int32_t d1 asm("d1") = (unsigned char)c;
    asm volatile("trap #15"
                 : "+r"(d0)
                 : "r"(d1)
                 : "d2", "a0", "a1", "memory");
}

static inline int iocs_b_getc(void)
{
    register int32_t d0 asm("d0") = 0x21;
    asm volatile("trap #15"
                 : "+r"(d0)
                 : : "d1", "d2", "a0", "a1", "memory");
    return d0 & 0xFF;
}

static inline int iocs_b_keysns(void)
{
    register int32_t d0 asm("d0") = 0x1C;
    asm volatile("trap #15"
                 : "+r"(d0)
                 : : "d1", "d2", "a0", "a1", "memory");
    return d0;
}

/* ── Crash-safe mode: skip TVRAM output to avoid double fault ───────────── *
 *
 * When the crash handler runs after an IOCS _B_PUTC fault, calling _B_PUTC
 * again would cause a double bus/address error (CPU halt).  Setting this
 * flag makes uart_putc a no-op for TVRAM; klogf output goes only via the
 * serial mirror (_OUT232C).
 */
int uart_tvram_inhibit;

/* ── VT100 → X68000 IOCS escape sequence converter ─────────────────────── *
 *
 * Intercepts ESC sequences before they reach IOCS _B_PUTC, parses them as
 * VT100 CSI, and converts supported sequences to IOCS calls:
 *
 *   ESC [ n A       → cursor up n        (_B_LOCATE)
 *   ESC [ n B       → cursor down n      (_B_LOCATE)
 *   ESC [ n C       → cursor forward n   (_B_LOCATE)
 *   ESC [ n D       → cursor back n      (_B_LOCATE)
 *   ESC [ row;col H → cursor position    (_B_LOCATE)
 *   ESC [ 2 J       → clear screen       (_B_CLR_ST)
 *   ESC [ 0 J       → clear to end       (_B_CLR_ED)
 *   ESC [ 0 K       → clear to EOL       (_B_CLR_AL)
 *   ESC [ ... m     → SGR (ignored — no TVRAM colour support)
 *   ESC [ ? ...     → private modes (ignored)
 *
 * Cursor position is tracked in software to support relative movements.
 */

#define VT_COLS  96   /* X68000 standard text screen width  */
#define VT_ROWS  31   /* X68000 standard text screen height */
#define VT_MAX_PARAMS 4

enum { ST_NORMAL = 0, ST_ESC, ST_CSI };

static int vt_state;
static int vt_params[VT_MAX_PARAMS];
static int vt_nparams;
static int vt_private;      /* '?' prefix seen */

/* Software cursor tracking */
static int cur_x, cur_y;

static int param(int idx, int def)
{
    if (idx < vt_nparams && vt_params[idx] > 0)
        return vt_params[idx];
    return def;
}

static void clamp_cursor(void)
{
    if (cur_x < 0) cur_x = 0;
    if (cur_y < 0) cur_y = 0;
    if (cur_x >= VT_COLS) cur_x = VT_COLS - 1;
    if (cur_y >= VT_ROWS) cur_y = VT_ROWS - 1;
}

static void locate(int x, int y)
{
    cur_x = x;
    cur_y = y;
    clamp_cursor();
    iocs_b_locate(cur_x, cur_y);
}

static void csi_dispatch(int final)
{
    if (vt_private)
        return;  /* ignore private modes (ESC[?...) */

    switch (final) {
    case 'A':  /* Cursor Up */
        locate(cur_x, cur_y - param(0, 1));
        break;
    case 'B':  /* Cursor Down */
        locate(cur_x, cur_y + param(0, 1));
        break;
    case 'C':  /* Cursor Forward */
        locate(cur_x + param(0, 1), cur_y);
        break;
    case 'D':  /* Cursor Back */
        locate(cur_x - param(0, 1), cur_y);
        break;
    case 'H': case 'f':  /* Cursor Position (1-based) */
        locate(param(1, 1) - 1, param(0, 1) - 1);
        break;
    case 'J':  /* Erase in Display */
        switch (param(0, 0)) {
        case 0: iocs_b_clr_ed(); break;   /* cursor to end */
        case 2:                            /* entire screen */
            iocs_b_clr_st();
            cur_x = cur_y = 0;
            break;
        }
        break;
    case 'K':  /* Erase in Line */
        if (param(0, 0) == 0)
            iocs_b_clr_al();  /* cursor to end of line */
        break;
    case 'm':  /* SGR — no colour support, ignore */
        break;
    case 'r':  /* DECSTBM — scroll region, ignore */
        break;
    default:
        break;  /* unknown — silently ignore */
    }
}

/* Feed one character through the VT100 converter.
 * Returns 1 if the character was consumed (part of an escape sequence).
 * Returns 0 if the character should be passed to _B_PUTC normally. */
static int vt_feed(char c)
{
    unsigned char ch = (unsigned char)c;

    switch (vt_state) {
    case ST_NORMAL:
        if (ch == 0x1Bu) {
            vt_state = ST_ESC;
            return 1;  /* consumed */
        }
        /* Track cursor for printable characters */
        if (ch >= 0x20u && ch <= 0x7Eu) {
            cur_x++;
            if (cur_x >= VT_COLS) {
                cur_x = 0;
                if (cur_y < VT_ROWS - 1)
                    cur_y++;
            }
        } else if (ch == '\n') {
            if (cur_y < VT_ROWS - 1)
                cur_y++;
        } else if (ch == '\r') {
            cur_x = 0;
        } else if (ch == '\b') {
            if (cur_x > 0)
                cur_x--;
        } else if (ch == '\t') {
            cur_x = (cur_x + 8) & ~7;
            if (cur_x >= VT_COLS)
                cur_x = VT_COLS - 1;
        }
        return 0;  /* pass through to _B_PUTC */

    case ST_ESC:
        if (ch == '[') {
            vt_state = ST_CSI;
            vt_nparams = 0;
            vt_private = 0;
            for (int i = 0; i < VT_MAX_PARAMS; i++)
                vt_params[i] = 0;
        } else if (ch == 'c') {
            /* RIS — full reset */
            vt_state = ST_NORMAL;
            iocs_b_clr_st();
            cur_x = cur_y = 0;
        } else {
            /* Unknown ESC+x — discard */
            vt_state = ST_NORMAL;
        }
        return 1;

    case ST_CSI:
        if (ch == '?') {
            vt_private = 1;
        } else if (ch >= '0' && ch <= '9') {
            if (vt_nparams == 0)
                vt_nparams = 1;
            vt_params[vt_nparams - 1] =
                vt_params[vt_nparams - 1] * 10 + (ch - '0');
        } else if (ch == ';') {
            if (vt_nparams < VT_MAX_PARAMS)
                vt_nparams++;
        } else if (ch >= 0x40u && ch <= 0x7Eu) {
            /* Final byte — dispatch */
            if (vt_nparams == 0 && ch != 'm')
                vt_nparams = 1;
            vt_state = ST_NORMAL;
            csi_dispatch(ch);
        } else {
            /* Unexpected byte — abort */
            vt_state = ST_NORMAL;
        }
        return 1;
    }

    vt_state = ST_NORMAL;
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void uart_init_console(void)
{
    iocs_b_clr_st();
    cur_x = cur_y = 0;
}


void uart_putc(char c)
{
    if (uart_tvram_inhibit)
        return;
    uint16_t sr = iocs_ipl_save();
    if (!vt_feed(c))
        iocs_b_putc(c);
    iocs_ipl_restore(sr);
}

void uart_puts(const char *s)
{
    uint16_t sr = iocs_ipl_save();
    while (*s) {
        if (!vt_feed(*s)) {
            if (*s == '\n')
                iocs_b_putc('\r');
            iocs_b_putc(*s);
        }
        s++;
    }
    iocs_ipl_restore(sr);
}

void uart_serial_putc(char c)
{
    uint16_t sr = iocs_ipl_save();
    if (c == '\n')
        iocs_out232c('\r');
    iocs_out232c(c);
    iocs_ipl_restore(sr);
}

/* ── RS-232C serial input via IOCS ───────────────────────────────────────── */

static inline int iocs_isns232c(void)
{
    register int32_t d0 asm("d0") = 0x32;
    asm volatile("trap #15"
                 : "+r"(d0)
                 : : "d1", "d2", "a0", "a1", "memory");
    return d0;
}

static inline int iocs_inp232c(void)
{
    register int32_t d0 asm("d0") = 0x33;
    asm volatile("trap #15"
                 : "+r"(d0)
                 : : "d1", "d2", "a0", "a1", "memory");
    return d0 & 0xFF;
}

int uart_serial_getc(void)
{
    uint16_t sr = iocs_ipl_save();
    int avail = iocs_isns232c();
    if (!avail) {
        iocs_ipl_restore(sr);
        return -1;
    }
    int c = iocs_inp232c();
    iocs_ipl_restore(sr);
    if (c > 0x7F)
        return -1;
    return c;
}

int uart_serial_rx_avail(void)
{
    uint16_t sr = iocs_ipl_save();
    int r = iocs_isns232c() ? 1 : 0;
    iocs_ipl_restore(sr);
    return r;
}

void uart_flush(void)
{
    /* IOCS _B_PUTC is synchronous — no TX buffer to flush */
}

void uart_reinit_133mhz(void)
{
    /* Not applicable — X68000 clock is fixed; IOCS needs no reinit */
}

void uart_init_irq(void)
{
    /* Phase X-2: enable MFP UART RX interrupt for non-blocking input.
     * For now, polling via _B_KEYSNS is used. */
}

int uart_getc(void)
{
    uint16_t sr = iocs_ipl_save();
    int avail = iocs_b_keysns();
    if (!avail) {
        iocs_ipl_restore(sr);
        return -1;
    }
    int c = iocs_b_getc();
    iocs_ipl_restore(sr);
    if (c > 0x7F)
        return -1;
    return c;
}

int uart_rx_avail(void)
{
    uint16_t sr = iocs_ipl_save();
    int r = iocs_b_keysns() ? 1 : 0;
    iocs_ipl_restore(sr);
    return r;
}

/*
 * uart_rx_avail_hw — non-IOCS keyboard availability check
 *
 * Checks the IOCS key input buffer directly in memory, avoiding any
 * IOCS TRAP #15 call.  This is safe to call from any context (ISR,
 * idle loop, thread) because it only reads a single word.
 *
 * X68000 IOCS keyboard buffer work area (system area, ROM v1.00):
 *   $0800    16 bytes  BITSNS key press-down state (groups 0-F)
 *   $0810.b  LED illumination status
 *   $0811.b  Shift key press state
 *   $0812.w  Key buffer data byte count (0 = empty)
 *   $0814.l  Key buffer write pointer
 *   $0818.l  Key buffer read pointer
 *   $081C    64 words  Key buffer ring storage
 *
 * _B_KEYSNS returns non-zero when the byte count at $0812 is non-zero.
 * We replicate that check with a plain memory read.
 *
 * Fallback: also checks MFP USART RSR bit 7 (Buffer Full) at 0xE8802B
 * in case a raw scan code is pending before the keyboard ISR runs.
 */
int uart_rx_avail_hw(void)
{
    /* Check IOCS key buffer byte count */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    volatile uint16_t *key_count = (volatile uint16_t *)0x0812u;
    if (*key_count != 0)
        return 1;
#pragma GCC diagnostic pop

    /* Fallback: MFP USART RSR bit 7 (raw scan code pending) */
    volatile uint8_t *rsr = (volatile uint8_t *)(0xE88001u + 21u * 2u);
    return (*rsr & 0x80u) ? 1 : 0;
}

/*
 * uart_serial_rx_avail_hw — non-IOCS RS-232C RX availability check
 *
 * Reads SCC (Z8530) channel B RR0 directly.  Bit 0 = Rx Char Available.
 * X68000 SCC register map (byte-wide at odd addresses):
 *   $E98001  Channel B command (RR0 on read)
 *   $E98003  Channel B data
 *   $E98005  Channel A command
 *   $E98007  Channel A data
 *
 * Safe to call from any context (no IOCS TRAP #15).
 */
int uart_serial_rx_avail_hw(void)
{
    volatile uint8_t *scc_rr0 = (volatile uint8_t *)0xE98001u;
    return (*scc_rr0 & 0x01u) ? 1 : 0;
}

void uart_print_hex32(uint32_t v)
{
    uart_puts("0x");
    for (int i = 7; i >= 0; i--) {
        unsigned nibble = (v >> (i * 4)) & 0xFu;
        uart_putc(nibble < 10u ? (char)('0' + nibble) : (char)('a' + nibble - 10u));
    }
}

void uart_print_dec(uint32_t v)
{
    char buf[10];
    int  i = 0;
    if (v == 0u) { uart_putc('0'); return; }
    while (v > 0u) { buf[i++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (--i >= 0) uart_putc(buf[i]);
}

/* Debug output — on X68000 use serial port (RS-232C via IOCS) */
void uart_debug_putc(char c) { uart_serial_putc(c); }
void uart_debug_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_serial_putc('\r');
        uart_serial_putc(*s++);
    }
}
void uart_debug_hex32(uint32_t v)
{
    uart_debug_puts("0x");
    for (int i = 7; i >= 0; i--) {
        unsigned nibble = (v >> (i * 4)) & 0xFu;
        uart_debug_putc(nibble < 10u ? (char)('0' + nibble)
                                     : (char)('a' + nibble - 10u));
    }
}
void uart_debug_dec(uint32_t v)
{
    char buf[10];
    int  i = 0;
    if (v == 0u) { uart_debug_putc('0'); return; }
    while (v > 0u) { buf[i++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (--i >= 0) uart_debug_putc(buf[i]);
}
void uart_debug_dump(void) { uart_debug_puts("[UART] (X68000 — IOCS serial)\n"); }
