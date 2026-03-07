/*
 * tty.c — Multi-TTY driver with line discipline
 *
 * Supports multiple independent TTY instances, each with its own backend,
 * line buffer, termios, and foreground process group.
 *
 *   tty_devs[0] — /dev/ttyS0 (UART, always available)
 *   tty_devs[1] — /dev/tty1  (fbcon+kbd on PicoCalc, NULL if no display)
 *
 * Each open of a /dev/ttyXX device allocates a struct file with
 * f->priv = &tty_devs[idx], so tty_read/tty_write dispatch to the
 * correct instance.
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

/* ── Per-instance TTY state ───────────────────────────────────────────────── */

#define LINE_BUF_SIZE  128

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

typedef struct {
    /* I/O backend callbacks */
    void (*out)(char c);
    void (*out_flush)(void);
    int  (*in)(void);
    int  (*in_avail)(void);
    int  (*win_cols)(void);
    int  (*win_rows)(void);

    /* Line discipline state */
    char line_buf[LINE_BUF_SIZE];
    uint16_t line_pos;
    volatile uint8_t line_ready;

    /* Termios settings */
    struct kernel_termios termios;

    /* Foreground process group (for job control) */
    int32_t fg_pgrp;

    /* Session leader PID that owns this TTY (set by TIOCSCTTY) */
    pid_t session;
} tty_dev_t;

static tty_dev_t tty_devs[TTY_MAX] = {
    [TTY_SERIAL] = {
        .out       = uart_putc,
        .out_flush = NULL,
        .in        = uart_getc,
        .in_avail  = uart_rx_avail,
        .win_cols  = NULL,
        .win_rows  = NULL,
        .termios   = {
            .c_iflag = ICRNL | IXON,
            .c_oflag = OPOST | ONLCR,
            .c_cflag = CFLAG_DEFAULT,
            .c_lflag = LFLAG_DEFAULT,
            .c_line  = 0,
            .c_cc    = { 0 },
        },
        .fg_pgrp = 0,
    },
    [TTY_DISPLAY] = {
        /* All NULL — pico1calc registers fbcon backend via tty_set_backend() */
        .termios = {
            .c_iflag = ICRNL | IXON,
            .c_oflag = OPOST | ONLCR,
            .c_cflag = CFLAG_DEFAULT,
            .c_lflag = LFLAG_DEFAULT,
            .c_line  = 0,
            .c_cc    = { 0 },
        },
        .fg_pgrp = 0,
    },
};

/* ── tty ioctl numbers ─────────────────────────────────────────────────────── */

#define TCGETS      0x5401u
#define TCSETS      0x5402u
#define TCSETSW     0x5403u
#define TCSETSF     0x5404u
#define TIOCGWINSZ  0x5413u
#define TIOCSCTTY   0x540Eu
#define TIOCGPGRP   0x540Fu
#define TIOCSPGRP   0x5410u
#define TIOCSWINSZ  0x5414u
#define TIOCGSID    0x5429u

/* ── Backend setup ─────────────────────────────────────────────────────────── */

void tty_set_backend(int idx, const tty_backend_t *be)
{
    if ((unsigned)idx >= TTY_MAX)
        return;
    tty_dev_t *t = &tty_devs[idx];
    t->out       = be->putc;
    t->out_flush = be->flush;
    t->in        = be->getc;
    t->in_avail  = be->rx_avail;
    t->win_cols  = be->get_cols;
    t->win_rows  = be->get_rows;
}

void *tty_get_dev(int idx)
{
    if ((unsigned)idx >= TTY_MAX)
        return &tty_devs[0];
    return &tty_devs[idx];
}

/* ── Output helper (putc + optional flush) ────────────────────────────────── */

static inline void dev_echo_flush(tty_dev_t *t)
{
    if (t->out_flush)
        t->out_flush();
}

/* ── SIGINT delivery ───────────────────────────────────────────────────────── */

static void dev_send_signal(tty_dev_t *t, int sig)
{
    int woke = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        pcb_t *p = &proc_table[i];
        if (p->state == PROC_FREE)
            continue;
        /* Match foreground process group.  When fg_pgrp == 0 (no job
         * control — hush without CONFIG_HUSH_JOB never calls tcsetpgrp),
         * signal all non-init processes: on a single-terminal system
         * without job control everything is "foreground". */
        if (t->fg_pgrp != 0) {
            if ((int32_t)p->pgid != t->fg_pgrp)
                continue;
        } else {
            if (p->pid <= 1)
                continue;   /* skip idle (0) and init (1) */
        }
        p->sig_pending |= (1u << (uint32_t)sig);
        /* Wake the process so it can handle the signal */
        if (p->state == PROC_BLOCKED) {
            p->state = PROC_RUNNABLE;
            p->wait_channel = NULL;
            woke = 1;
        } else if (p->state == PROC_SLEEPING) {
            p->state = PROC_RUNNABLE;
            woke = 1;
        }
    }
    /* Trigger context switch so woken process handles the signal promptly */
    if (woke)
        sched_yield();
}

/* ── tty_write ─────────────────────────────────────────────────────────────── */

static long tty_write(struct file *f, const char *buf, size_t n)
{
    tty_dev_t *t = (tty_dev_t *)f->priv;
    if (!t || !t->out)
        return -(long)ENODEV;

    int opost = (t->termios.c_oflag & OPOST) != 0;

    for (size_t i = 0u; i < n; i++) {
        if (opost && buf[i] == '\n' && (t->termios.c_oflag & ONLCR))
            t->out('\r');    /* NL → CR+NL when OPOST|ONLCR set */
        t->out(buf[i]);
    }
    dev_echo_flush(t);
    return (long)n;
}

/* ── tty_read: canonical (cooked) mode ─────────────────────────────────────── */

static long tty_read_canon(tty_dev_t *t, char *buf, size_t n)
{
    /* Drain all available characters from input */
    while (!t->line_ready) {
        int c = t->in();
        if (c < 0) {
            /* No data — check for pending signal before blocking */
            if (current->sig_pending & ~current->sig_blocked)
                return -(long)EINTR;
            /* Block via svc_restart: re-executes this syscall when woken */
            current->wait_channel = t;
            current->state = PROC_BLOCKED;
            svc_restart[core_id()] = 1;
            sched_yield();
            return 0;  /* ignored — SVC restores original args */
        }

        /* ICRNL: map CR to NL */
        if ((t->termios.c_iflag & ICRNL) && c == '\r')
            c = '\n';

        /* ISIG: signal characters */
        if (t->termios.c_lflag & ISIG) {
            if (c == CTRL_C) {   /* Ctrl-C → SIGINT */
                dev_send_signal(t, SIGINT);
                /* Discard line buffer, echo ^C */
                if (t->termios.c_lflag & ECHO) {
                    t->out('^');
                    t->out('C');
                    t->out('\r');
                    t->out('\n');
                    dev_echo_flush(t);
                }
                t->line_pos = 0;
                return -(long)EINTR;
            }
        }

        /* Backspace / DEL */
        if (c == ASCII_DEL || c == ASCII_BS) {
            if (t->line_pos > 0) {
                t->line_pos--;
                if (t->termios.c_lflag & ECHO) {
                    t->out('\b');
                    t->out(' ');
                    t->out('\b');
                    dev_echo_flush(t);
                }
            }
            continue;
        }

        /* Ctrl-U: kill line */
        if (c == CTRL_U) {
            while (t->line_pos > 0) {
                t->line_pos--;
                if (t->termios.c_lflag & ECHO) {
                    t->out('\b');
                    t->out(' ');
                    t->out('\b');
                }
            }
            dev_echo_flush(t);
            continue;
        }

        /* Ctrl-D: EOF / flush */
        if (c == CTRL_D) {
            if (t->line_pos == 0)
                return 0;           /* EOF */
            t->line_ready = 1;     /* flush what we have */
            break;
        }

        /* Newline: complete the line */
        if (c == '\n') {
            if (t->line_pos < LINE_BUF_SIZE)
                t->line_buf[t->line_pos++] = (char)c;
            if (t->termios.c_lflag & ECHO) {
                t->out('\r');
                t->out('\n');
                dev_echo_flush(t);
            }
            t->line_ready = 1;
            break;
        }

        /* Regular character */
        if (t->line_pos < LINE_BUF_SIZE) {
            t->line_buf[t->line_pos++] = (char)c;
            if (t->termios.c_lflag & ECHO) {
                t->out((char)c);
                dev_echo_flush(t);
            }
        } else {
            /* Line buffer full — ring bell to notify user */
            t->out('\a');
            dev_echo_flush(t);
        }
    }

    /* Deliver buffered data to user */
    size_t avail = t->line_pos;
    if (avail > n)
        avail = n;
    __builtin_memcpy(buf, t->line_buf, avail);

    /* Shift remaining data (if partial read) */
    if (avail < t->line_pos) {
        for (uint16_t i = 0; i < t->line_pos - (uint16_t)avail; i++)
            t->line_buf[i] = t->line_buf[(uint16_t)avail + i];
        t->line_pos -= (uint16_t)avail;
    } else {
        t->line_pos = 0;
        t->line_ready = 0;
    }

    return (long)avail;
}

/* ── tty_read: raw mode ────────────────────────────────────────────────────── */

static long tty_read_raw(tty_dev_t *t, char *buf, size_t n)
{
    (void)n;
    int c = t->in();
    if (c < 0) {
        /* Check for pending signal before blocking */
        if (current->sig_pending & ~current->sig_blocked)
            return -(long)EINTR;
        /* Block via svc_restart */
        current->wait_channel = t;
        current->state = PROC_BLOCKED;
        svc_restart[core_id()] = 1;
        sched_yield();
        return 0;  /* ignored — SVC restores original args */
    }

    /* ICRNL: map CR to NL */
    if ((t->termios.c_iflag & ICRNL) && c == '\r')
        c = '\n';

    /* ISIG in raw mode too */
    if (t->termios.c_lflag & ISIG) {
        if (c == CTRL_C) {
            dev_send_signal(t, SIGINT);
            return -(long)EINTR;
        }
    }

    if (t->termios.c_lflag & ECHO) {
        if (c == '\n' && (t->termios.c_oflag & OPOST) &&
            (t->termios.c_oflag & ONLCR))
            t->out('\r');
        t->out((char)c);
        dev_echo_flush(t);
    }

    buf[0] = (char)c;
    return 1;   /* raw mode: return after first char (VMIN=1) */
}

/* ── tty_read dispatcher ───────────────────────────────────────────────────── */

static long tty_read(struct file *f, char *buf, size_t n)
{
    tty_dev_t *t = (tty_dev_t *)f->priv;
    if (!t || !t->in)
        return -(long)ENODEV;
    if (n == 0)
        return 0;

    if (t->termios.c_lflag & ICANON)
        return tty_read_canon(t, buf, n);
    else
        return tty_read_raw(t, buf, n);
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
    tty_dev_t *t = (tty_dev_t *)f->priv;
    if (!t)
        return -ENOTTY;

    switch (cmd) {
    case TCGETS:
        __builtin_memcpy(arg, &t->termios, sizeof(t->termios));
        return 0;
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        __builtin_memcpy(&t->termios, arg, sizeof(t->termios));
        return 0;
    case TIOCGWINSZ: {
        struct winsize *ws = (struct winsize *)arg;
        ws->ws_col    = (uint16_t)(t->win_cols ? t->win_cols()
                                               : TTY_DEFAULT_COLS);
        ws->ws_row    = (uint16_t)(t->win_rows ? t->win_rows()
                                               : TTY_DEFAULT_ROWS);
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        return 0;
    }
    case TIOCSWINSZ:
        return 0;   /* ignore — fixed-size terminal */
    case TIOCGPGRP: {
        int32_t *pg = (int32_t *)arg;
        *pg = t->fg_pgrp;
        return 0;
    }
    case TIOCSPGRP: {
        const int32_t *pg = (const int32_t *)arg;
        t->fg_pgrp = *pg;
        return 0;
    }
    case TIOCSCTTY: {
        /* Set controlling terminal.  Record the caller's session ID. */
        pcb_t *p = current;
        if (!p)
            return -EPERM;
        /* Only a session leader may acquire a ctty */
        if (p->pid != p->sid)
            return -EPERM;
        t->session = p->sid;
        t->fg_pgrp = (int32_t)p->pgid;
        return 0;
    }
    case TIOCGSID: {
        pid_t *sp = (pid_t *)arg;
        *sp = t->session;
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

/* ── tty_poll ──────────────────────────────────────────────────────────────── */

static int tty_poll(struct file *f)
{
    tty_dev_t *t = (tty_dev_t *)f->priv;
    int mask = POLLOUT;   /* write never blocks */

    if (t && (t->line_ready || (t->in_avail && t->in_avail())))
        mask |= POLLIN;

    return mask;
}

/* ── ISR notification hooks ────────────────────────────────────────────────── */

void tty_rx_notify(int idx)
{
    if ((unsigned)idx >= TTY_MAX)
        return;
    sched_wakeup(&tty_devs[idx]);
}

int tty_signal_intr(int idx)
{
    if ((unsigned)idx >= TTY_MAX)
        return 0;
    tty_dev_t *t = &tty_devs[idx];

    if (!(t->termios.c_lflag & ISIG))
        return 0;   /* signals disabled — leave 0x03 for tty_read */
    /* Echo ^C + newline (like Linux n_tty when ECHO/ECHOCTL are set) */
    if (t->termios.c_lflag & ECHO) {
        if (t->out) {
            t->out('^');
            t->out('C');
            t->out('\r');
            t->out('\n');
        }
        dev_echo_flush(t);
    }
    /* Discard any partial canonical line buffer */
    t->line_pos = 0;
    t->line_ready = 0;
    dev_send_signal(t, SIGINT);
    return 1;   /* consumed — ISR should not queue this byte */
}

void tty_set_fg_pgrp(int idx, int pgid)
{
    if ((unsigned)idx >= TTY_MAX)
        return;
    tty_devs[idx].fg_pgrp = (int32_t)pgid;
}

/* ── Static file objects ────────────────────────────────────────────────────── */

const struct file_ops tty_fops = {
    tty_read, tty_write, tty_close, tty_ioctl, tty_poll
};

struct file tty_stdin  = { &tty_fops, NULL, O_RDONLY, 1u };
struct file tty_stdout = { &tty_fops, NULL, O_WRONLY, 1u };
struct file tty_stderr = { &tty_fops, NULL, O_WRONLY, 1u };
