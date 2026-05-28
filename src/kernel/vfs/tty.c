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

#include "kernel/vfs/tty.h"

#include <stddef.h>
#include <stdint.h>

#include "common/errno.h"                 /* ENOTTY, EINTR, EAGAIN */
#include "common/fcntl.h"                 /* O_NONBLOCK */
#include "common/signal.h"                /* SIGINT */
#include "common/termios.h"               /* struct termios, flag bits */
#include "kernel/common/core/proc_info.h" /* proc_table, PROC_MAX, PROC_FREE */
#include "kernel/common/mod/mod_core.h"
#include "kernel/common/spinlock.h"
#include "kernel/vfs/file.h"

/* ── Termios defaults not in common header ─────────────────────────────────
 */

/* c_cflag */
#define CFLAG_DEFAULT 0x00BFu /* CS8 | CREAD | HUPCL | B38400 */

/* c_lflag */
#define LFLAG_DEFAULT \
  0x8A3Bu /* ECHO|ECHOE|ECHOK|ICANON|ISIG|IEXTEN|ECHOCTL|ECHOKE */

/* ── Control character codes ────────────────────────────────────────────────
 */

#define CTRL_C 0x03    /* ETX — interrupt (SIGINT)      */
#define CTRL_D 0x04    /* EOT — end of file             */
#define CTRL_U 0x15    /* NAK — kill line               */
#define ASCII_BS 0x08  /* backspace                     */
#define ASCII_DEL 0x7F /* delete                        */

/* ── Default terminal size ──────────────────────────────────────────────────
 */

#define TTY_DEFAULT_ROWS 25
#define TTY_DEFAULT_COLS 80

/* ── Per-instance TTY state ─────────────────────────────────────────────────
 */

#define LINE_BUF_SIZE 128

typedef struct {
  /* I/O backend callbacks */
  int (*out)(char c, void (*notify)(void)); /* 1=ok, 0=full */
  void (*out_flush)(void);
  int (*in)(void);
  int (*in_avail)(void);
  int (*win_cols)(void);
  int (*win_rows)(void);
  void (*set_winsize)(int cols, int rows);

  /* TX-ready callback for this TTY — passed to out() on blocking */
  void (*tx_wakeup)(void);

  /* Line discipline state */
  char line_buf[LINE_BUF_SIZE];
  uint16_t line_pos;
  volatile uint8_t line_ready;

  /* Termios settings */
  struct termios termios;

  /* Foreground process group (for job control) */
  int32_t fg_pgrp;

  /* Session leader PID that owns this TTY (set by TIOCSCTTY) */
  pid_t session;

  /* Stable address used as the wait_channel for blocked writers — the
   * backend TX-ready callback calls sched_wakeup(&t->tx_page) to rouse
   * us.  The address is all that matters; no write-progress state is
   * kept here since tty_write loops internally across sched_switch(). */
  page_id_t tx_page;
} tty_dev_t;

/* Per-instance state.  All TTY backends — including TTY_SERIAL — are
 * registered at boot by the target's klog_init_logger() (or by
 * vfs_notify(VFS_EVENT_LATE_INIT) for targets with elaborate dual-
 * backend setups like x68k).  tty.c does not pin any specific
 * transport (UART, USB Serial JTAG, BIOS console, …); the tty_dev_t
 * fields populated by tty_set_backend() are what carries the link. */
static tty_dev_t tty_devs[TTY_MAX] = {
    [TTY_SERIAL] =
        {
            .termios =
                {
                    .c_iflag = ICRNL | IXON,
                    .c_oflag = OPOST | ONLCR,
                    .c_cflag = CFLAG_DEFAULT,
                    .c_lflag = LFLAG_DEFAULT,
                    .c_line = 0,
                    .c_cc = {0},
                },
            .fg_pgrp = 0,
        },
    [TTY_DISPLAY] =
        {
            .termios =
                {
                    .c_iflag = ICRNL | IXON,
                    .c_oflag = OPOST | ONLCR,
                    .c_cflag = CFLAG_DEFAULT,
                    .c_lflag = LFLAG_DEFAULT,
                    .c_line = 0,
                    .c_cc = {0},
                },
            .fg_pgrp = 0,
        },
};

/* Primary console TTY index — changed by tty_set_console() before first fork */
static int console_tty_idx = TTY_SERIAL;

/* ── tty ioctl numbers (beyond common/termios.h) ────────────────────────────
 */

#define TIOCSCTTY 0x540Eu
#define TIOCGPGRP 0x540Fu
#define TIOCSPGRP 0x5410u
#define TIOCGSID 0x5429u

/* ── Backend setup ───────────────────────────────────────────────────────────
 */

/* TX-ready callbacks — one per TTY, called from the backend ISR */
static void tx_ready_0(void) { mod_core.sched_wakeup(&tty_devs[0].tx_page); }
static void tx_ready_1(void) { mod_core.sched_wakeup(&tty_devs[1].tx_page); }
static void (*const tx_ready_fn[TTY_MAX])(void) = {tx_ready_0, tx_ready_1};

void tty_set_backend(int idx, const tty_backend_t *be) {
  if ((unsigned)idx >= TTY_MAX) return;
  tty_dev_t *t = &tty_devs[idx];
  t->out = be->putc;
  t->out_flush = be->flush;
  t->in = be->getc;
  t->in_avail = be->rx_avail;
  t->win_cols = be->get_cols;
  t->win_rows = be->get_rows;
  t->set_winsize = be->set_winsize;
  t->tx_wakeup = tx_ready_fn[idx];
}

void *tty_get_dev(int idx) {
  if ((unsigned)idx >= TTY_MAX) return &tty_devs[0];
  return &tty_devs[idx];
}

int tty_backend_ready(int idx) {
  if ((unsigned)idx >= TTY_MAX) return 0;
  return tty_devs[idx].out != NULL;
}

void tty_set_console(int idx) {
  if ((unsigned)idx < TTY_MAX) console_tty_idx = idx;
}

void *tty_get_console_dev(void) {
  /* tx_wakeup is wired by tty_set_backend() at registration time. */
  return tty_get_dev(console_tty_idx);
}

int tty_raw_getc(int idx) {
  if ((unsigned)idx >= TTY_MAX) return -1;
  tty_dev_t *t = &tty_devs[idx];
  if (!t->in) return -1;
  return t->in();
}

int tty_raw_putc(int idx, char c, void (*notify)(void)) {
  if ((unsigned)idx >= TTY_MAX) return 0;
  tty_dev_t *t = &tty_devs[idx];
  if (!t->out) return 0;
  return t->out(c, notify);
}

static void tty_advance(page_id_t *page, uint16_t *off, size_t delta) {
  size_t pos = (size_t)*off + delta;

  *page += (page_id_t)(pos / PAGE_SIZE);
  *off = (uint16_t)(pos % PAGE_SIZE);
}

static void tty_copy_to_pages(page_id_t page, uint16_t off, const void *src,
                              size_t len) {
  const uint8_t *in = (const uint8_t *)src;

  while (len > 0) {
    size_t chunk = PAGE_SIZE - off;

    if (chunk > len) chunk = len;
    mod_core.page_write(page, off, in, (uint16_t)chunk);
    in += chunk;
    len -= chunk;
    tty_advance(&page, &off, chunk);
  }
}

/* ── Output helpers (echo + flush) ───────────────────────────────────────────
 */

/* Echo one character — retries if the backend is full.
 * Echo is single-character so busy-spin is acceptable. */
static inline void dev_echo(tty_dev_t *t, char c) { while (!t->out(c, NULL)); }

static inline void dev_echo_flush(tty_dev_t *t) {
  if (t->out_flush) t->out_flush();
}

/* ── SIGINT delivery ─────────────────────────────────────────────────────────
 */

static void dev_send_signal(tty_dev_t *t, int sig) {
  int woke = 0;
  uint32_t saved = spin_lock_irqsave(SPIN_PROC);
  for (uint32_t i = 0; i < PROC_MAX; i++) {
    pcb_t *p = &proc_table[i];
    if (p->state == PROC_FREE) continue;
    /* Match foreground process group.  When fg_pgrp == 0 (no job
     * control — hush without CONFIG_HUSH_JOB never calls tcsetpgrp),
     * signal all non-init processes: on a single-terminal system
     * without job control everything is "foreground". */
    if (t->fg_pgrp != 0) {
      if ((int32_t)p->pgid != t->fg_pgrp) continue;
    } else {
      if (p->pid <= 1) continue; /* skip idle (0) and init (1) */
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
  spin_unlock_irqrestore(SPIN_PROC, saved);
  /* Trigger context switch so woken process handles the signal promptly */
  if (woke) mod_core.sched_switch();
}

/* ── tty_write ───────────────────────────────────────────────────────────────
 *
 * Calls t->out(c, notify) per character.  If the backend returns 0 (full),
 * the TX-ready callback is registered atomically inside that same putc call.
 * We sleep (PROC_BLOCKED) on &t->tx_page; the ISR fires the callback which
 * wakes us.  The whole write completes inside this function — we loop across
 * sched_switch() and retry the put — so the syscall returns a real status
 * (bytes written, -EAGAIN under O_NONBLOCK, or -EINTR on signal).
 */

static long tty_write(struct file *f, page_id_t page, uint16_t off, size_t n) {
  tty_dev_t *t = (tty_dev_t *)f->priv;
  if (!t) return -(long)ENODEV;
  if (!t->out) return (long)n; /* no backend — silently discard */

  int opost = (t->termios.c_oflag & OPOST) != 0;
  int onlcr = opost && (t->termios.c_oflag & ONLCR);
  int nonblock = (f->flags & O_NONBLOCK) ? 1 : 0;

  size_t pos = 0;
  page_id_t cur_page = page;
  uint16_t cur_off = off;

  while (pos < n) {
    char c;
    mod_core.page_read(cur_page, cur_off, &c, 1);

    /* OPOST: expand \n → \r\n */
    if (onlcr && c == '\n') {
      while (!t->out('\r', t->tx_wakeup)) {
        if (nonblock) return pos > 0 ? (long)pos : -(long)EAGAIN;
        if (current->sig_pending & ~current->sig_blocked)
          return pos > 0 ? (long)pos : -(long)EINTR;
        mod_core.sched_sleep_current(&t->tx_page);
      }
    }
    while (!t->out(c, t->tx_wakeup)) {
      if (nonblock) return pos > 0 ? (long)pos : -(long)EAGAIN;
      if (current->sig_pending & ~current->sig_blocked)
        return pos > 0 ? (long)pos : -(long)EINTR;
      mod_core.sched_sleep_current(&t->tx_page);
    }
    pos++;
    tty_advance(&cur_page, &cur_off, 1);
  }

  dev_echo_flush(t);
  return (long)n;
}

/* ── tty_read: canonical (cooked) mode ───────────────────────────────────────
 */

static long tty_read_canon(tty_dev_t *t, int nonblock, page_id_t page,
                           uint16_t off, size_t n) {
  /* Drain all available characters from input */
  while (!t->line_ready) {
    int c = t->in();
    if (c < 0) {
      /* No data — caller asked O_NONBLOCK?  Tell them to retry. */
      if (nonblock) return -(long)EAGAIN;
      /* No data — check for pending signal before blocking */
      if (current->sig_pending & ~current->sig_blocked) return -(long)EINTR;
      /* Block and retry after wakeup.  We stay inside this function
       * across the context switch, so the syscall completes with a
       * real status when we eventually get data or a signal. */
      mod_core.sched_sleep_current(t);
      continue;
    }

    /* ICRNL: map CR to NL */
    if ((t->termios.c_iflag & ICRNL) && c == '\r') c = '\n';

    /* ISIG: signal characters */
    if (t->termios.c_lflag & ISIG) {
      if (c == CTRL_C) { /* Ctrl-C → SIGINT */
        dev_send_signal(t, SIGINT);
        /* Discard line buffer, echo ^C */
        if (t->termios.c_lflag & ECHO) {
          dev_echo(t, '^');
          dev_echo(t, 'C');
          dev_echo(t, '\r');
          dev_echo(t, '\n');
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
          dev_echo(t, '\b');
          dev_echo(t, ' ');
          dev_echo(t, '\b');
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
          dev_echo(t, '\b');
          dev_echo(t, ' ');
          dev_echo(t, '\b');
        }
      }
      dev_echo_flush(t);
      continue;
    }

    /* Ctrl-D: EOF / flush */
    if (c == CTRL_D) {
      if (t->line_pos == 0) return 0; /* EOF */
      t->line_ready = 1;              /* flush what we have */
      break;
    }

    /* Newline: complete the line */
    if (c == '\n') {
      if (t->line_pos < LINE_BUF_SIZE) t->line_buf[t->line_pos++] = (char)c;
      if (t->termios.c_lflag & ECHO) {
        dev_echo(t, '\r');
        dev_echo(t, '\n');
        dev_echo_flush(t);
      }
      t->line_ready = 1;
      break;
    }

    /* Regular character */
    if (t->line_pos < LINE_BUF_SIZE) {
      t->line_buf[t->line_pos++] = (char)c;
      if (t->termios.c_lflag & ECHO) {
        dev_echo(t, (char)c);
        dev_echo_flush(t);
      }
    } else {
      /* Line buffer full — ring bell to notify user */
      dev_echo(t, '\a');
      dev_echo_flush(t);
    }
  }

  /* Deliver buffered data to user */
  size_t avail = t->line_pos;
  if (avail > n) avail = n;
  tty_copy_to_pages(page, off, t->line_buf, avail);

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

/* ── tty_read: raw mode ──────────────────────────────────────────────────────
 */

static long tty_read_raw(tty_dev_t *t, int nonblock, page_id_t page,
                         uint16_t off, size_t n) {
  (void)n;
  int c;
  /* Wait loop: block in sched_switch() and retry t->in() after each
   * wakeup until we actually have a byte.  The syscall completes with
   * a real status — no syscall_set_restart + trap-IP-rewind dance — so
   * kernel-side callers (DOS/CP/M/Human68k bridges) see the correct
   * return value. */
  for (;;) {
    c = t->in();
    if (c >= 0) break;
    if (nonblock) return -(long)EAGAIN;
    if (current->sig_pending & ~current->sig_blocked) return -(long)EINTR;
    mod_core.sched_sleep_current(t);
  }

  /* ICRNL: map CR to NL */
  if ((t->termios.c_iflag & ICRNL) && c == '\r') c = '\n';

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
      dev_echo(t, '\r');
    dev_echo(t, (char)c);
    dev_echo_flush(t);
  }

  {
    char out = (char)c;
    mod_core.page_write(page, off, &out, 1);
  }
  return 1; /* raw mode: return after first char (VMIN=1) */
}

/* ── tty_read dispatcher ─────────────────────────────────────────────────────
 */

static long tty_read(struct file *f, page_id_t page, uint16_t off, size_t n) {
  tty_dev_t *t = (tty_dev_t *)f->priv;
  if (!t) return -(long)ENODEV;
  int nonblock = (f->flags & O_NONBLOCK) ? 1 : 0;
  if (!t->in) {
    if (nonblock) return -(long)EAGAIN;
    /* No backend — block until signalled.  Loop across sched_switch()
     * so the syscall completes with -EINTR rather than a phantom 0. */
    for (;;) {
      if (current->sig_pending & ~current->sig_blocked) return -(long)EINTR;
      mod_core.sched_sleep_current(t);
    }
  }
  if (n == 0) return 0;

  if (t->termios.c_lflag & ICANON)
    return tty_read_canon(t, nonblock, page, off, n);
  else
    return tty_read_raw(t, nonblock, page, off, n);
}

/* ── tty_close ───────────────────────────────────────────────────────────────
 */

static int tty_close(struct file *f) {
  (void)f;
  return 0; /* tty is never truly closed */
}

/* ── tty ioctl ───────────────────────────────────────────────────────────────
 */

static int tty_ioctl(struct file *f, uint32_t cmd, void *arg) {
  tty_dev_t *t = (tty_dev_t *)f->priv;
  if (!t) return -ENOTTY;

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
      ws->ws_col = (uint16_t)(t->win_cols ? t->win_cols() : TTY_DEFAULT_COLS);
      ws->ws_row = (uint16_t)(t->win_rows ? t->win_rows() : TTY_DEFAULT_ROWS);
      ws->ws_xpixel = 0;
      ws->ws_ypixel = 0;
      return 0;
    }
    case TIOCSWINSZ: {
      const struct winsize *ws = (const struct winsize *)arg;
      if (t->set_winsize) t->set_winsize(ws->ws_col, ws->ws_row);
      return 0;
    }
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
      if (!p) return -EPERM;
      /* Only a session leader may acquire a ctty */
      if (p->pid != p->sid) return -EPERM;
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

/* ── tty_poll ────────────────────────────────────────────────────────────────
 */

static int tty_poll(struct file *f) {
  tty_dev_t *t = (tty_dev_t *)f->priv;
  int mask = POLLOUT; /* write never blocks */

  if (t && (t->line_ready || (t->in_avail && t->in_avail()))) mask |= POLLIN;

  return mask;
}

/* ── ISR notification hooks ──────────────────────────────────────────────────
 */

void tty_rx_notify(int idx) {
  if ((unsigned)idx >= TTY_MAX) return;
  mod_core.sched_wakeup(&tty_devs[idx]);
}

void tty_poll_input(void) {
  for (unsigned i = 0; i < TTY_MAX; i++) {
    if (tty_devs[i].in_avail && tty_devs[i].in_avail()) tty_rx_notify((int)i);
  }
}

int tty_signal_intr(int idx) {
  if ((unsigned)idx >= TTY_MAX) return 0;
  tty_dev_t *t = &tty_devs[idx];

  if (!(t->termios.c_lflag & ISIG))
    return 0; /* signals disabled — leave 0x03 for tty_read */
  /* Echo ^C + newline (like Linux n_tty when ECHO/ECHOCTL are set) */
  if (t->termios.c_lflag & ECHO) {
    if (t->out) {
      dev_echo(t, '^');
      dev_echo(t, 'C');
      dev_echo(t, '\r');
      dev_echo(t, '\n');
    }
    dev_echo_flush(t);
  }
  /* Discard any partial canonical line buffer */
  t->line_pos = 0;
  t->line_ready = 0;
  dev_send_signal(t, SIGINT);
  return 1; /* consumed — ISR should not queue this byte */
}

void tty_set_fg_pgrp(int idx, int pgid) {
  if ((unsigned)idx >= TTY_MAX) return;
  tty_devs[idx].fg_pgrp = (int32_t)pgid;
}

/* ── Static file objects ──────────────────────────────────────────────────────
 */

const struct file_ops tty_fops = {tty_read, tty_write, tty_close, tty_ioctl,
                                  tty_poll};

struct file tty_stdin = {
    .ops = &tty_fops,
    .flags = O_RDONLY,
    .refcnt = 1u,
};

struct file tty_stdout = {
    .ops = &tty_fops,
    .flags = O_WRONLY,
    .refcnt = 1u,
};

struct file tty_stderr = {
    .ops = &tty_fops,
    .flags = O_WRONLY,
    .refcnt = 1u,
};
