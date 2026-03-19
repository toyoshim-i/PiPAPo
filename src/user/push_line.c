/*
 * push_line.c — PiPAPo μShell line editor (Phase 3)
 *
 * VT100/ANSI line editor with history and tab completion.
 * Operates in raw terminal mode (no ICANON, no ECHO).
 *
 * Linked with push.c for the full interactive shell.
 * Can be omitted for script-only builds.
 */

#include "syscall.h"
#include "push.h"

/* ── Termios ─────────────────────────────────────────────────────────── */

#define NCCS       19
#define TCGETS     0x5401u
#define TCSETS     0x5402u

/* c_iflag */
#define ICRNL      0x0100u
#define IXON       0x0400u

/* c_oflag */
#define OPOST      0x0001u
#define ONLCR      0x0004u

/* c_lflag */
#define ISIG       0x0001u
#define ICANON     0x0002u
#define ECHO_FLAG  0x0008u

struct termios {
    unsigned c_iflag;
    unsigned c_oflag;
    unsigned c_cflag;
    unsigned c_lflag;
    unsigned char c_line;
    unsigned char c_cc[NCCS];
};

/* Terminal window size */
#define TIOCGWINSZ 0x5413u

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

/* ── Signal numbers ──────────────────────────────────────────────────── */

#define SIGINT  2

/* ── String helpers (local, avoid linking push.c's static funcs) ─────── */

static int pl_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void pl_puts(int fd, const char *s)
{
    write(fd, s, pl_strlen(s));
}

static int pl_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static void pl_strcpy(char *dst, const char *src, int maxlen)
{
    int i = 0;
    while (src[i] && i < maxlen - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void pl_strncpy(char *dst, const char *src, int n, int maxlen)
{
    int i = 0;
    while (i < n && i < maxlen - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ── Terminal control ────────────────────────────────────────────────── */

static struct termios orig_termios;
static int raw_mode_active;

static void term_raw(void)
{
    struct termios t;
    ioctl(0, TCGETS, &t);
    orig_termios = t;

    /* Raw mode: no ICANON, no ECHO, keep ISIG for Ctrl-C,
     * keep OPOST|ONLCR for output \n → \r\n */
    t.c_iflag &= ~(ICRNL | IXON);
    t.c_lflag &= ~(ICANON | ECHO_FLAG);
    /* ISIG stays on so kernel delivers SIGINT on Ctrl-C */

    ioctl(0, TCSETS, &t);
    raw_mode_active = 1;
}

static void term_restore(void)
{
    if (raw_mode_active) {
        ioctl(0, TCSETS, &orig_termios);
        raw_mode_active = 0;
    }
}

static int term_cols(void)
{
    struct winsize ws;
    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

/* ── VT100 output helpers ────────────────────────────────────────────── */

/* Move cursor to column col (0-based) relative to prompt start */
static void cursor_to(int prompt_len, int col)
{
    /* Use absolute column positioning: \033[<n>G (1-based) */
    int abs_col = prompt_len + col + 1;
    char buf[16];
    buf[0] = '\033'; buf[1] = '[';
    int i = 2;
    if (abs_col >= 100) buf[i++] = '0' + abs_col / 100;
    if (abs_col >= 10)  buf[i++] = '0' + (abs_col / 10) % 10;
    buf[i++] = '0' + abs_col % 10;
    buf[i++] = 'G';
    write(1, buf, i);
}

/* Erase from cursor to end of line */
static void erase_eol(void)
{
    write(1, "\033[K", 3);
}

/* Refresh the line display from the cursor position */
static void refresh_line(const char *prompt, const char *buf, int len,
                         int pos, int prompt_len)
{
    /* Move to start, rewrite prompt + buffer, erase remainder */
    write(1, "\r", 1);
    pl_puts(1, prompt);
    write(1, buf, len);
    erase_eol();
    cursor_to(prompt_len, pos);
}

/* ── History ─────────────────────────────────────────────────────────── */

static char hist_ring[PUSH_HISTORY_MAX][PUSH_LINE_MAX];
static int  hist_count;    /* total entries stored */
static int  hist_head;     /* next write slot (ring index) */

void push_history_add(const char *line)
{
    if (!line[0]) return;

    /* Suppress duplicate consecutive entries */
    if (hist_count > 0) {
        int prev = (hist_head + PUSH_HISTORY_MAX - 1) % PUSH_HISTORY_MAX;
        if (pl_streq(hist_ring[prev], line)) return;
    }

    pl_strcpy(hist_ring[hist_head], line, PUSH_LINE_MAX);
    hist_head = (hist_head + 1) % PUSH_HISTORY_MAX;
    if (hist_count < PUSH_HISTORY_MAX) hist_count++;
}

static const char *hist_get(int idx)
{
    /* idx 0 = most recent, idx (hist_count-1) = oldest */
    if (idx < 0 || idx >= hist_count) return 0;
    int slot = (hist_head + PUSH_HISTORY_MAX - 1 - idx) % PUSH_HISTORY_MAX;
    return hist_ring[slot];
}

void push_history_list(int fd)
{
    for (int i = hist_count - 1; i >= 0; i--) {
        const char *e = hist_get(i);
        if (e) {
            pl_puts(fd, e);
            write(fd, "\n", 1);
        }
    }
}

/* ── Tab completion ──────────────────────────────────────────────────── */

/*
 * Extract the word being completed from buf[0..pos).
 * Returns start index of the word in buf.
 */
static int comp_word_start(const char *buf, int pos)
{
    int i = pos;
    while (i > 0 && buf[i - 1] != ' ' && buf[i - 1] != '\t')
        i--;
    return i;
}

/*
 * Split a partial path into directory and prefix.
 * "foo/bar" → dir="foo/", prefix="bar"
 * "bar"     → dir=".", prefix="bar"
 */
static void comp_split(const char *word, int wlen,
                       char *dir, int dir_size,
                       char *prefix, int prefix_size)
{
    int last_slash = -1;
    for (int i = 0; i < wlen; i++)
        if (word[i] == '/') last_slash = i;

    if (last_slash >= 0) {
        pl_strncpy(dir, word, last_slash + 1, dir_size);
        pl_strncpy(prefix, word + last_slash + 1,
                    wlen - last_slash - 1, prefix_size);
    } else {
        dir[0] = '.'; dir[1] = '\0';
        pl_strncpy(prefix, word, wlen, prefix_size);
    }
}

/*
 * Check if name starts with prefix (case-sensitive).
 */
static int starts_with(const char *name, const char *prefix)
{
    while (*prefix) {
        if (*name != *prefix) return 0;
        name++; prefix++;
    }
    return 1;
}

/*
 * Find common prefix length among all matches.
 */
static int common_prefix_len(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return i;
}

/*
 * Perform tab completion on buf at position pos.
 * Returns new line length. Updates *pos_out.
 * second_tab: 1 if this is a second consecutive Tab (list matches).
 */
static int do_complete(char *buf, int len, int pos, int *pos_out,
                       const char *prompt, int prompt_len, int second_tab)
{
    int wstart = comp_word_start(buf, pos);
    int wlen = pos - wstart;

    char dir[PATH_BUF];
    char prefix[PPAP_NAME_MAX + 1];
    comp_split(buf + wstart, wlen, dir, sizeof(dir),
               prefix, sizeof(prefix));
    int plen = pl_strlen(prefix);

    /* Scan directory for matches */
    int dfd = open(dir, O_RDONLY, 0);
    if (dfd < 0) { *pos_out = pos; return len; }

    char first_match[PPAP_NAME_MAX + 1];
    char common[PPAP_NAME_MAX + 1];
    int match_count = 0;
    int first_is_dir = 0;
    struct dirent de;

    while (getdents(dfd, &de, sizeof(de)) > 0) {
        if (de.d_name[0] == '.' && !prefix[0]) continue;  /* skip dotfiles */
        if (plen > 0 && !starts_with(de.d_name, prefix)) continue;

        if (match_count == 0) {
            pl_strcpy(first_match, de.d_name, sizeof(first_match));
            pl_strcpy(common, de.d_name, sizeof(common));
            first_is_dir = (de.d_type == DT_DIR);
        } else {
            /* Shrink common prefix */
            int cpl = common_prefix_len(common, de.d_name);
            common[cpl] = '\0';
        }
        match_count++;
    }
    close(dfd);

    if (match_count == 0) {
        *pos_out = pos;
        return len;
    }

    if (match_count == 1) {
        /* Single match — insert it */
        const char *insert = first_match + plen;
        int ilen = pl_strlen(insert);
        int suffix = (first_is_dir) ? 1 : 1;  /* '/' or ' ' */

        /* Check space */
        if (len + ilen + suffix >= PUSH_LINE_MAX) {
            *pos_out = pos;
            return len;
        }

        /* Shift tail right */
        for (int i = len; i >= pos; i--)
            buf[i + ilen + suffix] = buf[i];

        /* Insert match suffix */
        for (int i = 0; i < ilen; i++)
            buf[pos + i] = insert[i];
        buf[pos + ilen] = first_is_dir ? '/' : ' ';

        len += ilen + suffix;
        *pos_out = pos + ilen + suffix;
        buf[len] = '\0';
        refresh_line(prompt, buf, len, *pos_out, prompt_len);
        return len;
    }

    /* Multiple matches */
    int common_len = pl_strlen(common);
    int extra = common_len - plen;

    if (extra > 0) {
        /* Insert common prefix */
        if (len + extra >= PUSH_LINE_MAX) {
            *pos_out = pos;
            return len;
        }
        for (int i = len; i >= pos; i--)
            buf[i + extra] = buf[i];
        for (int i = 0; i < extra; i++)
            buf[pos + i] = common[plen + i];
        len += extra;
        *pos_out = pos + extra;
        buf[len] = '\0';
        refresh_line(prompt, buf, len, *pos_out, prompt_len);
        return len;
    }

    if (second_tab) {
        /* List all matches */
        write(1, "\r\n", 2);

        /* Re-scan directory to list */
        dfd = open(dir, O_RDONLY, 0);
        if (dfd >= 0) {
            int col = 0;
            int cols = term_cols();
            while (getdents(dfd, &de, sizeof(de)) > 0) {
                if (de.d_name[0] == '.' && !prefix[0]) continue;
                if (plen > 0 && !starts_with(de.d_name, prefix)) continue;
                int nlen = pl_strlen(de.d_name);
                if (col + nlen + 2 > cols && col > 0) {
                    write(1, "\r\n", 2);
                    col = 0;
                }
                pl_puts(1, de.d_name);
                if (de.d_type == DT_DIR) { write(1, "/", 1); nlen++; }
                /* Pad to 16-char columns */
                int pad = 16 - (nlen % 16);
                if (pad < 16) {
                    for (int i = 0; i < pad; i++) write(1, " ", 1);
                    col += nlen + pad;
                } else {
                    col += nlen;
                }
            }
            close(dfd);
            write(1, "\r\n", 2);
        }
        refresh_line(prompt, buf, len, pos, prompt_len);
    }

    *pos_out = pos;
    return len;
}

/* ── Prompt rendering ────────────────────────────────────────────────── */

/*
 * Render PS1 prompt with escape sequences.
 * Supported: \w (cwd), \u (user), \h (hostname), \$ (# or $)
 * Returns rendered length (for cursor positioning).
 */
static int render_prompt(char *out, int out_size)
{
    /* Try to get PS1 from environment — need access to push.c's env.
     * Since push_line.c is linked with push.c, we call env_get via
     * the extern declaration below. For now, use a default. */
    const char *ps1 = 0;

    /* We can't directly call push.c's static env_get.
     * push.c will set the prompt string and pass it to push_readline.
     * For now, use a simple default. */
    if (!ps1) ps1 = "push$ ";

    int i = 0;
    const char *p = ps1;
    while (*p && i < out_size - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'w': {
                /* Current directory */
                char cwd[PATH_BUF];
                if (getcwd(cwd, sizeof(cwd)) == 0) {
                    for (int j = 0; cwd[j] && i < out_size - 1; j++)
                        out[i++] = cwd[j];
                }
                break;
            }
            case 'u': {
                /* Username — just use "root" for now */
                const char *u = "root";
                for (int j = 0; u[j] && i < out_size - 1; j++)
                    out[i++] = u[j];
                break;
            }
            case 'h': {
                /* Hostname */
                char hn[32];
                int fd = open("/etc/hostname", O_RDONLY, 0);
                if (fd >= 0) {
                    int n = read(fd, hn, sizeof(hn) - 1);
                    close(fd);
                    if (n > 0) {
                        /* Strip trailing newline */
                        if (hn[n - 1] == '\n') n--;
                        for (int j = 0; j < n && i < out_size - 1; j++)
                            out[i++] = hn[j];
                    }
                } else {
                    out[i++] = '?';
                }
                break;
            }
            case '$':
                out[i++] = '$';  /* always $ (no root check) */
                break;
            default:
                out[i++] = *p;
                break;
            }
            p++;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return i;
}

/* ── SIGINT handler ──────────────────────────────────────────────────── */

static volatile int sigint_received;

static void sigint_handler(int sig)
{
    (void)sig;
    sigint_received = 1;
}

/* ── Main readline function ──────────────────────────────────────────── */

int push_readline(const char *prompt, char *buf, int size)
{
    char prompt_buf[128];
    int prompt_len;

    if (prompt) {
        pl_strcpy(prompt_buf, prompt, sizeof(prompt_buf));
        prompt_len = pl_strlen(prompt_buf);
    } else {
        prompt_len = render_prompt(prompt_buf, sizeof(prompt_buf));
    }

    /* Install SIGINT handler */
    sigint_received = 0;
    sigaction(SIGINT, (void *)(long)sigint_handler, 0);

    /* Enter raw mode */
    term_raw();

    /* Display prompt */
    pl_puts(1, prompt_buf);

    int len = 0;   /* current line length */
    int pos = 0;   /* cursor position */
    int hist_idx = -1;  /* -1 = current line, 0+ = history */
    char saved_line[PUSH_LINE_MAX];  /* saved current line when browsing history */
    saved_line[0] = '\0';
    buf[0] = '\0';
    int last_was_tab = 0;

    for (;;) {
        if (sigint_received) {
            /* Ctrl-C: discard line, print new prompt */
            sigint_received = 0;
            write(1, "^C\r\n", 4);
            term_restore();
            buf[0] = '\0';
            return 0;  /* return empty line, not EOF */
        }

        char c;
        int n = read(0, &c, 1);
        if (n <= 0) {
            term_restore();
            return -1;  /* EOF */
        }

        if (c == '\r' || c == '\n') {
            /* Enter — accept line */
            write(1, "\r\n", 2);
            buf[len] = '\0';
            term_restore();
            if (len > 0) push_history_add(buf);
            last_was_tab = 0;
            return len;
        }

        if (c == '\t') {
            /* Tab completion */
            buf[len] = '\0';
            len = do_complete(buf, len, pos, &pos, prompt_buf, prompt_len,
                              last_was_tab);
            last_was_tab = 1;
            continue;
        }
        last_was_tab = 0;

        if (c == 0x04) {
            /* Ctrl-D: EOF on empty line, delete char otherwise */
            if (len == 0) {
                term_restore();
                return -1;
            }
            /* Delete char at cursor */
            if (pos < len) {
                for (int i = pos; i < len - 1; i++) buf[i] = buf[i + 1];
                len--;
                buf[len] = '\0';
                refresh_line(prompt_buf, buf, len, pos, prompt_len);
            }
            continue;
        }

        if (c == 0x01) {
            /* Ctrl-A: beginning of line */
            pos = 0;
            cursor_to(prompt_len, pos);
            continue;
        }

        if (c == 0x05) {
            /* Ctrl-E: end of line */
            pos = len;
            cursor_to(prompt_len, pos);
            continue;
        }

        if (c == 0x02) {
            /* Ctrl-B: back one char */
            if (pos > 0) { pos--; cursor_to(prompt_len, pos); }
            continue;
        }

        if (c == 0x06) {
            /* Ctrl-F: forward one char */
            if (pos < len) { pos++; cursor_to(prompt_len, pos); }
            continue;
        }

        if (c == 0x0B) {
            /* Ctrl-K: kill to end of line */
            len = pos;
            buf[len] = '\0';
            erase_eol();
            continue;
        }

        if (c == 0x15) {
            /* Ctrl-U: kill to start of line */
            for (int i = 0; i < len - pos; i++) buf[i] = buf[pos + i];
            len -= pos;
            pos = 0;
            buf[len] = '\0';
            refresh_line(prompt_buf, buf, len, pos, prompt_len);
            continue;
        }

        if (c == 0x17) {
            /* Ctrl-W: kill word backward */
            int old_pos = pos;
            while (pos > 0 && (buf[pos - 1] == ' ' || buf[pos - 1] == '\t'))
                pos--;
            while (pos > 0 && buf[pos - 1] != ' ' && buf[pos - 1] != '\t')
                pos--;
            int delta = old_pos - pos;
            for (int i = pos; i + delta < len; i++) buf[i] = buf[i + delta];
            len -= delta;
            buf[len] = '\0';
            refresh_line(prompt_buf, buf, len, pos, prompt_len);
            continue;
        }

        if (c == 0x0C) {
            /* Ctrl-L: clear screen, redraw */
            write(1, "\033[2J\033[H", 7);
            refresh_line(prompt_buf, buf, len, pos, prompt_len);
            continue;
        }

        if (c == 0x08 || c == 0x7F) {
            /* Backspace / DEL: delete char before cursor */
            if (pos > 0) {
                for (int i = pos - 1; i < len - 1; i++) buf[i] = buf[i + 1];
                pos--;
                len--;
                buf[len] = '\0';
                refresh_line(prompt_buf, buf, len, pos, prompt_len);
            }
            continue;
        }

        if (c == 0x10) {
            /* Ctrl-P: previous history */
            goto hist_prev;
        }

        if (c == 0x0E) {
            /* Ctrl-N: next history */
            goto hist_next;
        }

        if (c == '\033') {
            /* Escape sequence */
            char seq[4];
            int sn = read(0, &seq[0], 1);
            if (sn <= 0) continue;
            if (seq[0] == '[') {
                sn = read(0, &seq[1], 1);
                if (sn <= 0) continue;
                switch (seq[1]) {
                case 'A':  /* Up arrow */
                hist_prev:
                    if (hist_idx + 1 < hist_count) {
                        if (hist_idx == -1) {
                            /* Save current line */
                            pl_strcpy(saved_line, buf, PUSH_LINE_MAX);
                        }
                        hist_idx++;
                        const char *h = hist_get(hist_idx);
                        if (h) {
                            pl_strcpy(buf, h, size);
                            len = pl_strlen(buf);
                            pos = len;
                            refresh_line(prompt_buf, buf, len, pos, prompt_len);
                        }
                    }
                    continue;
                case 'B':  /* Down arrow */
                hist_next:
                    if (hist_idx >= 0) {
                        hist_idx--;
                        if (hist_idx == -1) {
                            pl_strcpy(buf, saved_line, size);
                        } else {
                            const char *h = hist_get(hist_idx);
                            if (h) pl_strcpy(buf, h, size);
                        }
                        len = pl_strlen(buf);
                        pos = len;
                        refresh_line(prompt_buf, buf, len, pos, prompt_len);
                    }
                    continue;
                case 'C':  /* Right arrow */
                    if (pos < len) { pos++; cursor_to(prompt_len, pos); }
                    continue;
                case 'D':  /* Left arrow */
                    if (pos > 0) { pos--; cursor_to(prompt_len, pos); }
                    continue;
                case 'H':  /* Home */
                    pos = 0;
                    cursor_to(prompt_len, pos);
                    continue;
                case 'F':  /* End */
                    pos = len;
                    cursor_to(prompt_len, pos);
                    continue;
                case '3':  /* Delete key: ESC [ 3 ~ */
                    sn = read(0, &seq[2], 1);
                    if (seq[2] == '~' && pos < len) {
                        for (int i = pos; i < len - 1; i++)
                            buf[i] = buf[i + 1];
                        len--;
                        buf[len] = '\0';
                        refresh_line(prompt_buf, buf, len, pos, prompt_len);
                    }
                    continue;
                case '1':  /* Home (alt): ESC [ 1 ~ */
                    sn = read(0, &seq[2], 1);
                    pos = 0;
                    cursor_to(prompt_len, pos);
                    continue;
                case '4':  /* End (alt): ESC [ 4 ~ */
                    sn = read(0, &seq[2], 1);
                    pos = len;
                    cursor_to(prompt_len, pos);
                    continue;
                }
            }
            continue;
        }

        /* Regular character — insert at cursor */
        if (c >= 0x20 && c < 0x7F && len < size - 1) {
            /* Shift tail right */
            for (int i = len; i > pos; i--) buf[i] = buf[i - 1];
            buf[pos] = c;
            pos++;
            len++;
            buf[len] = '\0';

            if (pos == len) {
                /* Appending at end — just write the char */
                write(1, &c, 1);
            } else {
                /* Inserting in middle — redraw from cursor */
                refresh_line(prompt_buf, buf, len, pos, prompt_len);
            }
        }
    }
}
