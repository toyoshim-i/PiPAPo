/*
 * tty.c — UART tty driver with line discipline
 *
 * Backed by uart_putc() / uart_getc() from drivers/uart.h.
 * After uart_init_irq() those calls are non-blocking ring-buffer operations;
 * the UART0_IRQ_Handler drains TX and fills RX asynchronously.
 *
 * tty_write: iterates the buffer; if OPOST is set, expands '\n' → '\r\n'.
 *
 * tty_read:  dispatches to canonical or raw mode based on ICANON flag.
 *   Canonical: buffers input line-by-line with echo and editing (BS, ^U, ^D).
 *   Raw: returns characters immediately (VMIN=1).
 *
 * tty_close: no-op — the tty is never really closed.
 *
 * ISIG: Ctrl-C sends SIGINT to the foreground process group.
 */

#include "tty.h"
#include "file.h"
#include "drivers/uart.h"
#include "../proc/proc.h"      /* proc_table, PROC_MAX, PROC_FREE */
#include "../proc/sched.h"     /* sched_wakeup, sched_yield */
#include "../syscall/syscall.h" /* svc_restart */
#include "../signal/signal.h"  /* SIGINT */
#include "../errno.h"          /* ENOTTY, EINTR */
#include <stddef.h>
#include <stdint.h>

/* ── Termios flag bits ─────────────────────────────────────────────────────── */

/* c_iflag */
#define ICRNL   0x0100u   /* map CR to NL on input */
#define IXON    0x0400u   /* enable XON/XOFF output control */

/* c_oflag */
#define OPOST   0x0001u   /* post-process output */
#define ONLCR   0x0004u   /* map NL to CR-NL on output */

/* c_cflag */
#define CFLAG_DEFAULT  0x00BFu   /* CS8 | CREAD | HUPCL | B38400 */

/* c_lflag */
#define ISIG    0x0001u   /* enable signals (INTR, QUIT, etc.) */
#define ICANON  0x0002u   /* canonical (line) mode */
#define ECHO    0x0008u   /* echo input characters */
#define LFLAG_DEFAULT  0x8A3Bu   /* ECHO|ECHOE|ECHOK|ICANON|ISIG|IEXTEN|ECHOCTL|ECHOKE */

/* ── Control character codes ──────────────────────────────────────────────── */

#define CTRL_C     0x03   /* ETX — interrupt (SIGINT)      */
#define CTRL_D     0x04   /* EOT — end of file             */
#define CTRL_U     0x15   /* NAK — kill line               */
#define ASCII_BS   0x08   /* backspace                     */
#define ASCII_DEL  0x7F   /* delete                        */

/* ── Default terminal size ────────────────────────────────────────────────── */

#define TTY_DEFAULT_ROWS  24
#define TTY_DEFAULT_COLS  80

/* ── c_cc array size ──────────────────────────────────────────────────────── */

#define NCCS  19   /* number of control characters (matches Linux ARM) */

/* ── Line discipline state ─────────────────────────────────────────────────── */

#define LINE_BUF_SIZE  128
static char line_buf[LINE_BUF_SIZE] __attribute__((section(".iobuf")));
static uint16_t line_pos;           /* next write position in line_buf */
static volatile uint8_t line_ready; /* 1 when complete line available */

/* ── tty ioctl numbers ─────────────────────────────────────────────────────── */

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
    uint8_t  c_cc[NCCS];
};

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

/* Default termios: canonical mode, echo on, signals enabled */
static struct kernel_termios tty_termios = {
    .c_iflag = ICRNL | IXON,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = CFLAG_DEFAULT,     /* CS8 | CREAD | HUPCL | B38400 */
    .c_lflag = LFLAG_DEFAULT,     /* ECHO|ECHOE|ECHOK|ICANON|ISIG|IEXTEN|ECHOCTL|ECHOKE */
    .c_line = 0,
    .c_cc = { 0 },
};

/* Foreground process group (for job control) */
static int32_t tty_fg_pgrp = 0;

/* ── SIGINT delivery ───────────────────────────────────────────────────────── */

static void tty_send_signal(int sig)
{
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        pcb_t *p = &proc_table[i];
        if (p->state != PROC_FREE &&
            (int32_t)p->pgid == tty_fg_pgrp) {
            p->sig_pending |= (1u << (uint32_t)sig);
            /* Wake the process so it can handle the signal */
            if (p->state == PROC_BLOCKED) {
                p->state = PROC_RUNNABLE;
                p->wait_channel = NULL;
            } else if (p->state == PROC_SLEEPING) {
                p->state = PROC_RUNNABLE;
            }
        }
    }
}

/* ── tty_write ─────────────────────────────────────────────────────────────── */

static long tty_write(struct file *f, const char *buf, size_t n)
{
    (void)f;
    int opost = (tty_termios.c_oflag & OPOST) != 0;

    for (size_t i = 0u; i < n; i++) {
        if (opost && buf[i] == '\n' && (tty_termios.c_oflag & ONLCR))
            uart_putc('\r');    /* NL → CR+NL when OPOST|ONLCR set */
        uart_putc(buf[i]);
    }
    return (long)n;
}

/* ── tty_read: canonical (cooked) mode ─────────────────────────────────────── */

static long tty_read_canon(char *buf, size_t n)
{
    /* Drain all available characters from UART ring buffer */
    while (!line_ready) {
        int c = uart_getc();
        if (c < 0) {
            /* No data — check for pending signal before blocking */
            if (current->sig_pending & ~current->sig_blocked)
                return -(long)EINTR;
            /* Block via svc_restart: re-executes this syscall when woken */
            current->wait_channel = &tty_stdin;
            current->state = PROC_BLOCKED;
            svc_restart = 1;
            sched_yield();
            return 0;  /* ignored — SVC restores original args */
        }

        /* ICRNL: map CR to NL */
        if ((tty_termios.c_iflag & ICRNL) && c == '\r')
            c = '\n';

        /* ISIG: signal characters */
        if (tty_termios.c_lflag & ISIG) {
            if (c == CTRL_C) {   /* Ctrl-C → SIGINT */
                tty_send_signal(SIGINT);
                /* Discard line buffer, echo ^C */
                if (tty_termios.c_lflag & ECHO) {
                    uart_putc('^');
                    uart_putc('C');
                    uart_putc('\r');
                    uart_putc('\n');
                }
                line_pos = 0;
                return -(long)EINTR;
            }
        }

        /* Backspace / DEL */
        if (c == ASCII_DEL || c == ASCII_BS) {
            if (line_pos > 0) {
                line_pos--;
                if (tty_termios.c_lflag & ECHO) {
                    uart_putc('\b');
                    uart_putc(' ');
                    uart_putc('\b');
                }
            }
            continue;
        }

        /* Ctrl-U: kill line */
        if (c == CTRL_U) {
            while (line_pos > 0) {
                line_pos--;
                if (tty_termios.c_lflag & ECHO) {
                    uart_putc('\b');
                    uart_putc(' ');
                    uart_putc('\b');
                }
            }
            continue;
        }

        /* Ctrl-D: EOF / flush */
        if (c == CTRL_D) {
            if (line_pos == 0)
                return 0;           /* EOF */
            line_ready = 1;         /* flush what we have */
            break;
        }

        /* Newline: complete the line */
        if (c == '\n') {
            if (line_pos < LINE_BUF_SIZE)
                line_buf[line_pos++] = (char)c;
            if (tty_termios.c_lflag & ECHO) {
                uart_putc('\r');
                uart_putc('\n');
            }
            line_ready = 1;
            break;
        }

        /* Regular character */
        if (line_pos < LINE_BUF_SIZE) {
            line_buf[line_pos++] = (char)c;
            if (tty_termios.c_lflag & ECHO)
                uart_putc((char)c);
        }
    }

    /* Deliver buffered data to user */
    size_t avail = line_pos;
    if (avail > n)
        avail = n;
    __builtin_memcpy(buf, line_buf, avail);

    /* Shift remaining data (if partial read) */
    if (avail < line_pos) {
        for (uint16_t i = 0; i < line_pos - (uint16_t)avail; i++)
            line_buf[i] = line_buf[(uint16_t)avail + i];
        line_pos -= (uint16_t)avail;
    } else {
        line_pos = 0;
        line_ready = 0;
    }

    return (long)avail;
}

/* ── tty_read: raw mode ────────────────────────────────────────────────────── */

static long tty_read_raw(char *buf, size_t n)
{
    (void)n;
    int c = uart_getc();
    if (c < 0) {
        /* Check for pending signal before blocking */
        if (current->sig_pending & ~current->sig_blocked)
            return -(long)EINTR;
        /* Block via svc_restart */
        current->wait_channel = &tty_stdin;
        current->state = PROC_BLOCKED;
        svc_restart = 1;
        sched_yield();
        return 0;  /* ignored — SVC restores original args */
    }

    /* ICRNL: map CR to NL */
    if ((tty_termios.c_iflag & ICRNL) && c == '\r')
        c = '\n';

    /* ISIG in raw mode too */
    if (tty_termios.c_lflag & ISIG) {
        if (c == CTRL_C) {
            tty_send_signal(SIGINT);
            return -(long)EINTR;
        }
    }

    if (tty_termios.c_lflag & ECHO) {
        if (c == '\n' && (tty_termios.c_oflag & OPOST) &&
            (tty_termios.c_oflag & ONLCR))
            uart_putc('\r');
        uart_putc((char)c);
    }

    buf[0] = (char)c;
    return 1;   /* raw mode: return after first char (VMIN=1) */
}

/* ── tty_read dispatcher ───────────────────────────────────────────────────── */

static long tty_read(struct file *f, char *buf, size_t n)
{
    (void)f;
    if (n == 0)
        return 0;

    if (tty_termios.c_lflag & ICANON)
        return tty_read_canon(buf, n);
    else
        return tty_read_raw(buf, n);
}

/* ── tty_close ─────────────────────────────────────────────────────────────── */

static int tty_close(struct file *f)
{
    (void)f;
    return 0;   /* tty is never truly closed */
}

/* ── tty ioctl ─────────────────────────────────────────────────────────────── */

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
        ws->ws_row = TTY_DEFAULT_ROWS;
        ws->ws_col = TTY_DEFAULT_COLS;
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
        return -ENOTTY;
    }
}

/* ── tty_poll ──────────────────────────────────────────────────────────────── */

static int tty_poll(struct file *f)
{
    (void)f;
    int mask = POLLOUT;   /* write never blocks */

    if (line_ready || uart_rx_avail())
        mask |= POLLIN;

    return mask;
}

/* ── ISR notification hooks ────────────────────────────────────────────────── */

void tty_rx_notify(void)
{
    sched_wakeup(&tty_stdin);
}

void tty_signal_intr(void)
{
    tty_send_signal(SIGINT);
}

/* ── Static file objects ────────────────────────────────────────────────────── */

static const struct file_ops tty_fops = {
    tty_read, tty_write, tty_close, tty_ioctl, tty_poll
};

struct file tty_stdin  = { &tty_fops, NULL, O_RDONLY, 1u };
struct file tty_stdout = { &tty_fops, NULL, O_WRONLY, 1u };
struct file tty_stderr = { &tty_fops, NULL, O_WRONLY, 1u };
