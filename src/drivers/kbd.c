/*
 * kbd.c — PicoCalc keyboard driver (STM32 co-processor via I2C1)
 *
 * The STM32 scans the 8×7 key matrix and queues events in a FIFO.
 * We poll the FIFO via I2C and translate keycodes to ASCII / VT100
 * escape sequences.
 *
 * The STM32 sends ASCII values (0x20–0x7E) for printable keys and
 * special codes (≥0x80) for navigation, function keys, and modifiers.
 * Modifier handling (Shift) is done by the STM32; we handle Ctrl
 * combinations on our side.
 */

#include "kbd.h"
#include "i2c.h"
#include "../kernel/klog.h"
#include <stdint.h>
#include <string.h>

/* ── STM32 I2C address and registers ────────────────────────────────────── */

#define KBD_ADDR        0x1F

#define REG_ID_VER      0x01   /* Firmware version (R)             */
#define REG_ID_KEY      0x04   /* FIFO status: bits[4:0] = count   */
#define REG_ID_FIF      0x09   /* FIFO data: 2 bytes [state, key]  */

/* Key states */
#define KEY_PRESSED     1
#define KEY_HOLD        2
#define KEY_RELEASED    3

/* ── STM32 keycodes (from PicoCalc keyboard.h) ─────────────────────────── */

#define KEY_BACKSPACE   0x08
#define KEY_TAB         0x09
#define KEY_ENTER       0x0A

#define KEY_F1          0x81
#define KEY_F2          0x82
#define KEY_F3          0x83
#define KEY_F4          0x84
#define KEY_F5          0x85
#define KEY_F6          0x86
#define KEY_F7          0x87
#define KEY_F8          0x88
#define KEY_F9          0x89
#define KEY_F10         0x90

#define KEY_POWER       0x91

#define KEY_MOD_ALT     0xA1
#define KEY_MOD_SHL     0xA2
#define KEY_MOD_SHR     0xA3
#define KEY_MOD_SYM     0xA4
#define KEY_MOD_CTRL    0xA5

#define KEY_ESC         0xB1
#define KEY_LEFT        0xB4
#define KEY_UP          0xB5
#define KEY_DOWN        0xB6
#define KEY_RIGHT       0xB7

#define KEY_CAPS_LOCK   0xC1

#define KEY_BREAK       0xD0
#define KEY_INSERT      0xD1
#define KEY_HOME        0xD2
#define KEY_DEL         0xD4
#define KEY_END         0xD5
#define KEY_PAGE_UP     0xD6
#define KEY_PAGE_DOWN   0xD7

/* ── Escape sequence buffer ─────────────────────────────────────────────── */

static char    seq_buf[8];
static uint8_t seq_pos;
static uint8_t seq_len;

/* Modifier state */
static uint8_t ctrl_held;

/* ── Helpers ────────────────────────────────────────────────────────────── */

/* Buffer a multi-byte escape sequence; returns first byte. */
static int buf_seq(const char *s)
{
    uint8_t len = 0;
    while (s[len])
        len++;
    /* Store full sequence, return first byte now */
    memcpy(seq_buf, s, len);
    seq_pos = 1;
    seq_len = len;
    return (uint8_t)seq_buf[0];
}

/* Translate a keycode to an output byte or buffered escape sequence.
 * Returns the first output byte, or -1 to suppress the event. */
static int translate(uint8_t state, uint8_t keycode)
{
    /* Track modifier state on press/release */
    if (keycode == KEY_MOD_CTRL) {
        ctrl_held = (state == KEY_PRESSED || state == KEY_HOLD);
        return -1;
    }

    /* Ignore modifiers and non-press events for other keys */
    if (keycode == KEY_MOD_ALT || keycode == KEY_MOD_SHL ||
        keycode == KEY_MOD_SHR || keycode == KEY_MOD_SYM)
        return -1;

    if (state != KEY_PRESSED)
        return -1;

    /* Ctrl + printable letter → control code */
    if (ctrl_held && keycode >= 0x20 && keycode <= 0x7E) {
        uint8_t c = keycode;
        if (c >= 'a' && c <= 'z')
            return c & 0x1F;
        if (c >= 'A' && c <= 'Z')
            return c & 0x1F;
        /* Ctrl+[ = ESC, Ctrl+\ = 0x1C, etc. */
        if (c >= '[' && c <= '_')
            return c & 0x1F;
        return -1;
    }

    /* Printable ASCII — pass through */
    if (keycode >= 0x20 && keycode <= 0x7E)
        return keycode;

    /* Standard control keys */
    switch (keycode) {
    case KEY_ENTER:     return '\r';
    case KEY_BACKSPACE: return 0x7F;
    case KEY_TAB:       return '\t';
    case KEY_ESC:       return 0x1B;

    /* Arrow keys → VT100 */
    case KEY_UP:        return buf_seq("\033[A");
    case KEY_DOWN:      return buf_seq("\033[B");
    case KEY_RIGHT:     return buf_seq("\033[C");
    case KEY_LEFT:      return buf_seq("\033[D");

    /* Navigation → xterm sequences */
    case KEY_HOME:      return buf_seq("\033[H");
    case KEY_END:       return buf_seq("\033[F");
    case KEY_INSERT:    return buf_seq("\033[2~");
    case KEY_DEL:       return buf_seq("\033[3~");
    case KEY_PAGE_UP:   return buf_seq("\033[5~");
    case KEY_PAGE_DOWN: return buf_seq("\033[6~");

    /* Function keys → xterm sequences */
    case KEY_F1:        return buf_seq("\033OP");
    case KEY_F2:        return buf_seq("\033OQ");
    case KEY_F3:        return buf_seq("\033OR");
    case KEY_F4:        return buf_seq("\033OS");
    case KEY_F5:        return buf_seq("\033[15~");
    case KEY_F6:        return buf_seq("\033[17~");
    case KEY_F7:        return buf_seq("\033[18~");
    case KEY_F8:        return buf_seq("\033[19~");
    case KEY_F9:        return buf_seq("\033[20~");
    case KEY_F10:       return buf_seq("\033[21~");

    default:
        return -1;
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void kbd_init(void)
{
    uint8_t ver = 0;
    int rc = i2c_read_reg(KBD_ADDR, REG_ID_VER, &ver, 1);
    if (rc < 0) {
        klog("KBD: STM32 not responding on I2C1\n");
        return;
    }
    klogf("KBD: STM32 firmware version %u\n", (uint32_t)ver);

    /* Drain any stale FIFO entries */
    for (int i = 0; i < 16; i++) {
        uint8_t status = 0;
        if (i2c_read_reg(KBD_ADDR, REG_ID_KEY, &status, 1) < 0)
            break;
        if ((status & 0x1F) == 0)
            break;
        uint8_t fifo[2];
        i2c_read_reg(KBD_ADDR, REG_ID_FIF, fifo, 2);
    }

    seq_len = 0;
    seq_pos = 0;
    ctrl_held = 0;
}

int kbd_poll(void)
{
    /* Return buffered escape sequence bytes first */
    if (seq_pos < seq_len)
        return (uint8_t)seq_buf[seq_pos++];

    seq_len = 0;
    seq_pos = 0;

    /* Check FIFO count */
    uint8_t status = 0;
    if (i2c_read_reg(KBD_ADDR, REG_ID_KEY, &status, 1) < 0)
        return -1;

    uint8_t count = status & 0x1F;
    if (count == 0)
        return -1;

    /* Read one event from FIFO */
    uint8_t fifo[2] = {0, 0};
    if (i2c_read_reg(KBD_ADDR, REG_ID_FIF, fifo, 2) < 0)
        return -1;

    return translate(fifo[0], fifo[1]);
}
