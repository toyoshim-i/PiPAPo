/*
 * Minimal curses implementation for Rogue on PPAP
 *
 * Translates curses calls to VT100/ANSI escape sequences.
 * The host terminal (minicom, screen, etc.) interprets the sequences.
 * PPAP's TTY driver passes them through transparently.
 */

#include "curses.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

/*
 * Globals
 */
WINDOW *stdscr = NULL;
WINDOW *curscr = NULL;
int LINES = 24;
int COLS  = 80;

static struct termios orig_termios;
static int termios_saved = 0;
static int is_endwin = 0;

/* Output buffer — batch escape sequences for efficiency */
static char outbuf[4096];
static int outpos = 0;

static void
flush_out(void)
{
    if (outpos > 0) {
        write(STDOUT_FILENO, outbuf, outpos);
        outpos = 0;
    }
}

static void
put_raw(const char *s, int len)
{
    while (len > 0) {
        int space = (int)sizeof(outbuf) - outpos;
        if (space <= 0) {
            flush_out();
            space = (int)sizeof(outbuf);
        }
        int n = len < space ? len : space;
        memcpy(outbuf + outpos, s, n);
        outpos += n;
        s += n;
        len -= n;
    }
}

static void
put_str(const char *s)
{
    put_raw(s, strlen(s));
}

/* Emit ESC[<n1>;<n2>H */
static void
emit_goto(int row, int col)
{
    char buf[16];
    int len = 0;
    buf[len++] = '\033';
    buf[len++] = '[';
    /* row+1 */
    if (row + 1 >= 10)
        buf[len++] = '0' + (row + 1) / 10;
    buf[len++] = '0' + (row + 1) % 10;
    buf[len++] = ';';
    /* col+1 */
    if (col + 1 >= 100)
        buf[len++] = '0' + (col + 1) / 100;
    if (col + 1 >= 10)
        buf[len++] = '0' + ((col + 1) / 10) % 10;
    buf[len++] = '0' + (col + 1) % 10;
    buf[len++] = 'H';
    put_raw(buf, len);
}

/*
 * Window allocation
 */
static WINDOW *
alloc_win(int nlines, int ncols, int begy, int begx, chtype *shared_buf)
{
    WINDOW *win = calloc(1, sizeof(WINDOW));
    if (!win)
        return NULL;

    win->_maxy = nlines;
    win->_maxx = ncols;
    win->_begy = begy;
    win->_begx = begx;
    win->_cury = 0;
    win->_curx = 0;
    win->_attrs = 0;
    win->_clear = 1;
    win->_leaveok = 0;
    win->_keypad = 0;
    win->_is_sub = 0;

    if (shared_buf) {
        win->_buf = shared_buf;
        win->_is_sub = 1;
    } else {
        win->_buf = calloc(nlines * ncols, sizeof(chtype));
        if (!win->_buf) {
            free(win);
            return NULL;
        }
        /* Fill with spaces */
        for (int i = 0; i < nlines * ncols; i++)
            win->_buf[i] = ' ';
    }
    win->_disp = NULL;
    return win;
}

/*
 * initscr — initialize the screen
 */
WINDOW *
initscr(void)
{
    /* Save and set raw terminal mode */
    if (!termios_saved && tcgetattr(STDIN_FILENO, &orig_termios) == 0)
        termios_saved = 1;

    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) == 0) {
        t.c_iflag &= ~(unsigned)(ICRNL | IXON);
        t.c_oflag &= ~(unsigned)(OPOST);
        t.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG | IEXTEN);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }

    /* Allocate stdscr */
    stdscr = alloc_win(LINES, COLS, 0, 0, NULL);
    if (!stdscr)
        return NULL;

    /* Allocate display buffer for diff-based refresh */
    stdscr->_disp = calloc(LINES * COLS, sizeof(chtype));
    if (stdscr->_disp) {
        for (int i = 0; i < LINES * COLS; i++)
            stdscr->_disp[i] = ' ';
    }

    /* curscr tracks the physical cursor state */
    curscr = alloc_win(LINES, COLS, 0, 0, NULL);

    /* Clear the terminal */
    put_str("\033[2J\033[H");
    flush_out();

    is_endwin = 0;
    return stdscr;
}

/*
 * endwin — restore terminal
 */
int
endwin(void)
{
    if (stdscr) {
        /* Reset attributes, show cursor, move to bottom */
        put_str("\033[0m\033[?25h");
        emit_goto(LINES - 1, 0);
        put_str("\r\n");
        flush_out();
    }

    if (termios_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);

    is_endwin = 1;
    return OK;
}

int
isendwin(void)
{
    return is_endwin;
}

/*
 * Window management
 */
WINDOW *
newwin(int nlines, int ncols, int begin_y, int begin_x)
{
    if (nlines == 0) nlines = LINES - begin_y;
    if (ncols == 0)  ncols = COLS - begin_x;
    return alloc_win(nlines, ncols, begin_y, begin_x, NULL);
}

WINDOW *
subwin(WINDOW *orig, int nlines, int ncols, int begin_y, int begin_x)
{
    if (!orig)
        return NULL;
    if (nlines == 0) nlines = orig->_maxy - (begin_y - orig->_begy);
    if (ncols == 0)  ncols = orig->_maxx - (begin_x - orig->_begx);
    /* Share the parent's buffer */
    WINDOW *win = alloc_win(nlines, ncols, begin_y, begin_x, orig->_buf);
    return win;
}

int
delwin(WINDOW *win)
{
    if (!win)
        return ERR;
    if (!win->_is_sub)
        free(win->_buf);
    free(win->_disp);
    free(win);
    return OK;
}

int
mvwin(WINDOW *win, int y, int x)
{
    if (!win)
        return ERR;
    win->_begy = y;
    win->_begx = x;
    return OK;
}

/*
 * Output functions
 */
int
wmove(WINDOW *win, int y, int x)
{
    if (!win)
        return ERR;
    if (y < 0 || y >= win->_maxy || x < 0 || x >= win->_maxx)
        return ERR;
    win->_cury = y;
    win->_curx = x;
    return OK;
}

int
waddch(WINDOW *win, chtype ch)
{
    if (!win)
        return ERR;
    if (win->_cury < 0 || win->_cury >= win->_maxy)
        return ERR;
    if (win->_curx < 0 || win->_curx >= win->_maxx)
        return ERR;

    chtype c = (ch & A_CHARTEXT) | win->_attrs;
    win->_buf[win->_cury * win->_maxx + win->_curx] = c;

    win->_curx++;
    if (win->_curx >= win->_maxx) {
        win->_curx = 0;
        win->_cury++;
        if (win->_cury >= win->_maxy)
            win->_cury = win->_maxy - 1;
    }
    return OK;
}

int
waddstr(WINDOW *win, const char *str)
{
    if (!win || !str)
        return ERR;
    while (*str)
        waddch(win, (chtype)(unsigned char)*str++);
    return OK;
}

int
wclrtoeol(WINDOW *win)
{
    if (!win)
        return ERR;
    for (int x = win->_curx; x < win->_maxx; x++)
        win->_buf[win->_cury * win->_maxx + x] = ' ' | win->_attrs;
    return OK;
}

int
wclear(WINDOW *win)
{
    if (!win)
        return ERR;
    for (int i = 0; i < win->_maxy * win->_maxx; i++)
        win->_buf[i] = ' ';
    win->_cury = 0;
    win->_curx = 0;
    win->_clear = 1;
    return OK;
}

int
werase(WINDOW *win)
{
    if (!win)
        return ERR;
    for (int i = 0; i < win->_maxy * win->_maxx; i++)
        win->_buf[i] = ' ';
    win->_cury = 0;
    win->_curx = 0;
    return OK;
}

/*
 * wrefresh — render window to terminal using diff against displayed state
 *
 * For stdscr: compares against _disp buffer, only emits changed cells.
 * For other windows: overlays onto stdscr's buffer and triggers stdscr refresh.
 */
int
wrefresh(WINDOW *win)
{
    if (!win)
        return ERR;

    /* If this is not stdscr, copy into stdscr's buffer and refresh stdscr */
    if (win != stdscr && win != curscr && stdscr) {
        for (int y = 0; y < win->_maxy; y++) {
            int sy = y + win->_begy;
            if (sy < 0 || sy >= LINES)
                continue;
            for (int x = 0; x < win->_maxx; x++) {
                int sx = x + win->_begx;
                if (sx < 0 || sx >= COLS)
                    continue;
                stdscr->_buf[sy * COLS + sx] = win->_buf[y * win->_maxx + x];
            }
        }
        if (win->_clear)
            stdscr->_clear = 1;
        win->_clear = 0;
        return wrefresh(stdscr);
    }

    /* Refreshing stdscr (or curscr) */
    if (win->_clear || !stdscr->_disp) {
        /* Full repaint */
        put_str("\033[0m\033[2J\033[H");
        int last_attr = 0;
        for (int y = 0; y < LINES; y++) {
            emit_goto(y, 0);
            for (int x = 0; x < COLS; x++) {
                chtype c = stdscr->_buf[y * COLS + x];
                int attr = c & A_ATTRIBUTES;
                if (attr != last_attr) {
                    put_str(attr & A_STANDOUT ? "\033[7m" : "\033[0m");
                    last_attr = attr;
                }
                char ch = c & A_CHARTEXT;
                if (ch == 0) ch = ' ';
                put_raw(&ch, 1);
                if (stdscr->_disp)
                    stdscr->_disp[y * COLS + x] = c;
            }
        }
        if (last_attr)
            put_str("\033[0m");
        win->_clear = 0;
    } else {
        /* Diff-based update */
        int last_attr = -1;
        int cursor_y = -1, cursor_x = -1;
        for (int y = 0; y < LINES; y++) {
            for (int x = 0; x < COLS; x++) {
                int idx = y * COLS + x;
                chtype c = stdscr->_buf[idx];
                if (c == stdscr->_disp[idx])
                    continue;
                /* Need to update this cell */
                if (cursor_y != y || cursor_x != x)
                    emit_goto(y, x);
                int attr = c & A_ATTRIBUTES;
                if (attr != last_attr) {
                    put_str(attr & A_STANDOUT ? "\033[7m" : "\033[0m");
                    last_attr = attr;
                }
                char ch = c & A_CHARTEXT;
                if (ch == 0) ch = ' ';
                put_raw(&ch, 1);
                stdscr->_disp[idx] = c;
                cursor_y = y;
                cursor_x = x + 1;
            }
        }
        if (last_attr > 0)
            put_str("\033[0m");
    }

    /* Position the physical cursor */
    if (!stdscr->_leaveok)
        emit_goto(stdscr->_cury, stdscr->_curx);

    /* Update curscr's position tracking */
    if (curscr) {
        curscr->_cury = stdscr->_cury;
        curscr->_curx = stdscr->_curx;
    }

    flush_out();
    return OK;
}

int
wprintw(WINDOW *win, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return waddstr(win, buf);
}

int
ppap_printw(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return waddstr(stdscr, buf);
}

int
ppap_mvprintw(int y, int x, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    wmove(stdscr, y, x);
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return waddstr(stdscr, buf);
}

int
mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    wmove(win, y, x);
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return waddstr(win, buf);
}

int
mvwaddch(WINDOW *win, int y, int x, chtype ch)
{
    wmove(win, y, x);
    return waddch(win, ch);
}

chtype
mvwinch(WINDOW *win, int y, int x)
{
    if (!win || y < 0 || y >= win->_maxy || x < 0 || x >= win->_maxx)
        return (chtype)ERR;
    return win->_buf[y * win->_maxx + x];
}

int
wstandout(WINDOW *win)
{
    if (!win)
        return ERR;
    win->_attrs |= A_STANDOUT;
    return OK;
}

int
wstandend(WINDOW *win)
{
    if (!win)
        return ERR;
    win->_attrs = 0;
    return OK;
}

/*
 * wgetnstr — read a string from the terminal with echo and line editing
 */
int
wgetnstr(WINDOW *win, char *str, int n)
{
    int pos = 0;

    if (!win || !str || n <= 0)
        return ERR;

    while (pos < n - 1) {
        int ch = getch();
        if (ch == '\n' || ch == '\r')
            break;
        if (ch == '\b' || ch == 127) {
            if (pos > 0) {
                pos--;
                /* Erase on screen */
                if (win->_curx > 0) {
                    win->_curx--;
                    waddch(win, ' ');
                    win->_curx--;
                }
            }
            continue;
        }
        if (ch >= ' ' && ch < 127) {
            str[pos++] = ch;
            waddch(win, ch);
            wrefresh(win);
        }
    }
    str[pos] = '\0';
    return OK;
}

/*
 * getch — read one character, with optional arrow key parsing
 */
int
getch(void)
{
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1)
        return ERR;

    /* Not escape — return directly */
    if (c != 27)
        return c;

    /* Try to read escape sequence.  Set a short timeout. */
    struct termios t, saved;
    tcgetattr(STDIN_FILENO, &saved);
    t = saved;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 1;  /* 100ms timeout */
    tcsetattr(STDIN_FILENO, TCSANOW, &t);

    unsigned char seq[4];
    int n = read(STDIN_FILENO, &seq[0], 1);
    if (n <= 0) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        return 27;  /* bare ESC */
    }

    if (seq[0] == '[' || seq[0] == 'O') {
        n = read(STDIN_FILENO, &seq[1], 1);
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        if (n <= 0)
            return 27;
        switch (seq[1]) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        case '5':
            /* Read trailing ~ */
            read(STDIN_FILENO, &seq[2], 1);
            return KEY_PPAGE;
        case '6':
            read(STDIN_FILENO, &seq[2], 1);
            return KEY_NPAGE;
        case '1':
            read(STDIN_FILENO, &seq[2], 1);
            return KEY_HOME;
        case '4':
            read(STDIN_FILENO, &seq[2], 1);
            return KEY_END;
        default:
            return seq[1];
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    /* ESC + something else — return ESC, push back not possible */
    return 27;
}

/*
 * Mode control
 */
int
raw(void)
{
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0)
        return ERR;
    t.c_iflag &= ~(unsigned)(ICRNL | IXON);
    t.c_oflag &= ~(unsigned)(OPOST);
    t.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG | IEXTEN);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    return OK;
}

int
noecho(void)
{
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0)
        return ERR;
    t.c_lflag &= ~(unsigned)ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    return OK;
}

int
keypad(WINDOW *win, int bf)
{
    if (win)
        win->_keypad = bf;
    return OK;
}

int
nocbreak(void)
{
    /* In our shim: restore VMIN=1, VTIME=0 (cancel halfdelay) */
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0)
        return ERR;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    return OK;
}

int
halfdelay(int tenths)
{
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0)
        return ERR;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = tenths;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    return OK;
}

/*
 * Window flags
 */
int
clearok(WINDOW *win, int bf)
{
    if (!win)
        return ERR;
    win->_clear = bf;
    return OK;
}

int
idlok(WINDOW *win, int bf)
{
    (void)win;
    (void)bf;
    return OK;
}

int
leaveok(WINDOW *win, int bf)
{
    if (!win)
        return ERR;
    win->_leaveok = bf;
    return OK;
}

int
touchwin(WINDOW *win)
{
    if (!win)
        return ERR;
    win->_clear = 1;
    return OK;
}

/*
 * mvcur — move the physical cursor
 */
int
mvcur(int oldrow, int oldcol, int newrow, int newcol)
{
    (void)oldrow;
    (void)oldcol;
    emit_goto(newrow, newcol);
    flush_out();
    if (curscr) {
        curscr->_cury = newrow;
        curscr->_curx = newcol;
    }
    return OK;
}

/*
 * unctrl — return printable representation of a character
 */
static char unctrl_buf[8];

char *
unctrl(chtype ch)
{
    unsigned char c = ch & 0x7F;
    if (c < ' ') {
        unctrl_buf[0] = '^';
        unctrl_buf[1] = c + '@';
        unctrl_buf[2] = '\0';
    } else if (c == 127) {
        unctrl_buf[0] = '^';
        unctrl_buf[1] = '?';
        unctrl_buf[2] = '\0';
    } else {
        unctrl_buf[0] = c;
        unctrl_buf[1] = '\0';
    }
    return unctrl_buf;
}

int
erasechar(void)
{
    return '\b';
}

int
killchar(void)
{
    return 21;  /* Ctrl-U */
}

int
baudrate(void)
{
    /* Return a high baud rate so rogue doesn't enter slow-terminal mode */
    return 115200;
}

int
flushinp(void)
{
    /* Discard any pending input */
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    return OK;
}

/*
 * xcrypt stub — md_crypt() calls this but wizard mode (MASTER) is
 * disabled so it's never reached at runtime.  Avoids linking the
 * 71 KB DES implementation from xcrypt.c.
 */
char *
xcrypt(const char *key, const char *setting)
{
    (void)key;
    (void)setting;
    return "";
}
