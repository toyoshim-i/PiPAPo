/*
 * tty.c — UART tty driver (struct file_ops implementation)
 *
 * Backed by uart_putc() / uart_getc() from drivers/uart.h.
 * After uart_init_irq() those calls are non-blocking ring-buffer operations;
 * the UART0_IRQ_Handler drains TX and fills RX asynchronously.
 *
 * tty_write: iterates the buffer, expands '\n' → '\r\n' for terminal compat,
 *            and calls uart_putc() for each byte.
 *
 * tty_read:  reads up to n bytes from the RX ring, stopping on '\n'
 *            (line-buffered).  Spins with WFI if no data is available yet
 *            so SysTick can preempt and other threads can run.
 *            On QEMU (no UART RX IRQ) the WFI wakes on the next SysTick.
 *
 * tty_close: no-op — the tty is never really closed.
 */

#include "tty.h"
#include "file.h"
#include "drivers/uart.h"
#include <stddef.h>
#include <stdint.h>

/* ── tty file operations ────────────────────────────────────────────────────── */

static long tty_write(struct file *f, const char *buf, size_t n)
{
    (void)f;
    for (size_t i = 0u; i < n; i++) {
        if (buf[i] == '\n')
            uart_putc('\r');    /* expand LF → CR+LF for terminal compatibility */
        uart_putc(buf[i]);
    }
    return (long)n;
}

static long tty_read(struct file *f, char *buf, size_t n)
{
    (void)f;
    size_t i = 0u;
    while (i < n) {
        int c = uart_getc();
        if (c < 0) {
            if (i)
                break;              /* already have bytes — return them */
            __asm__ volatile("wfi"); /* sleep until UART RX IRQ or SysTick */
            continue;
        }
        buf[i++] = (char)c;
        if (c == '\n')
            break;                  /* line-buffered read */
    }
    return (long)i;
}

static int tty_close(struct file *f)
{
    (void)f;
    return 0;   /* tty is never truly closed */
}

/* ── tty ioctl ─────────────────────────────────────────────────────────────── */

/* Linux termios ioctl numbers */
#define TCGETS      0x5401u
#define TCSETS      0x5402u
#define TCSETSW     0x5403u
#define TCSETSF     0x5404u
#define TIOCGWINSZ  0x5413u
#define TIOCGPGRP   0x540Fu
#define TIOCSPGRP   0x5410u
#define TIOCSWINSZ  0x5414u

/* Minimal termios for musl compatibility (matches Linux ARM layout) */
struct kernel_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
};

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

/* Default termios: canonical mode, echo on, typical baud */
static struct kernel_termios tty_termios = {
    .c_iflag = 0x0500,   /* ICRNL | IXON */
    .c_oflag = 0x0005,   /* OPOST | ONLCR */
    .c_cflag = 0x00BF,   /* CS8 | CREAD | HUPCL | B38400 */
    .c_lflag = 0x8A3B,   /* ECHO | ECHOE | ECHOK | ICANON | ISIG | IEXTEN | ECHOCTL | ECHOKE */
    .c_line = 0,
    .c_cc = { 0 },
};

/* Foreground process group (for job control) */
static int32_t tty_fg_pgrp = 0;

static int tty_ioctl(struct file *f, uint32_t cmd, void *arg)
{
    (void)f;

    switch (cmd) {
    case TCGETS:
        __builtin_memcpy(arg, &tty_termios, sizeof(tty_termios));
        return 0;
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        __builtin_memcpy(&tty_termios, arg, sizeof(tty_termios));
        return 0;
    case TIOCGWINSZ: {
        struct winsize *ws = (struct winsize *)arg;
        ws->ws_row = 24;
        ws->ws_col = 80;
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        return 0;
    }
    case TIOCSWINSZ:
        return 0;   /* ignore — fixed-size terminal */
    case TIOCGPGRP: {
        int32_t *pg = (int32_t *)arg;
        *pg = tty_fg_pgrp;
        return 0;
    }
    case TIOCSPGRP: {
        const int32_t *pg = (const int32_t *)arg;
        tty_fg_pgrp = *pg;
        return 0;
    }
    default:
        return -25;   /* ENOTTY */
    }
}

/* ── Static file objects ────────────────────────────────────────────────────── */

static const struct file_ops tty_fops = {
    tty_read, tty_write, tty_close, tty_ioctl
};

struct file tty_stdin  = { &tty_fops, NULL, O_RDONLY, 1u };
struct file tty_stdout = { &tty_fops, NULL, O_WRONLY, 1u };
struct file tty_stderr = { &tty_fops, NULL, O_WRONLY, 1u };
