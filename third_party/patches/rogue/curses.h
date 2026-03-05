/*
 * Minimal curses shim for Rogue on PPAP
 *
 * Translates curses calls to VT100/ANSI escape sequences.
 * Only implements the subset that Rogue 5.4.4 actually uses.
 */

#ifndef PPAP_CURSES_H
#define PPAP_CURSES_H

#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>

/*
 * Types
 */
typedef unsigned int chtype;
typedef chtype attr_t;

#define CURSES 1
/* Do NOT define NCURSES_VERSION — mdport.c checks it to access cur_term */

/*
 * Attributes — stored in upper bits of chtype
 */
#define A_CHARTEXT  0x000000FFU
#define A_ATTRIBUTES 0xFFFFFF00U
#define A_STANDOUT  0x00000100U
#define A_REVERSE   A_STANDOUT

/*
 * Key codes — returned by getch() when keypad mode is on
 */
#define ERR         (-1)
#define OK          0

#define KEY_DOWN    0x102
#define KEY_UP      0x103
#define KEY_LEFT    0x104
#define KEY_RIGHT   0x105
#define KEY_HOME    0x106
#define KEY_END     0x168
#define KEY_NPAGE   0x152
#define KEY_PPAGE   0x153
#define KEY_A1      0x1C1
#define KEY_A3      0x1C3
#define KEY_B2      0x1C5
#define KEY_C1      0x1C7
#define KEY_C3      0x1C9
#define KEY_BACKSPACE 0x107

/*
 * Boolean values (rogue uses these)
 */
#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/*
 * term.h stubs — rogue's mdport.c checks clr_eol for clear-to-eol support.
 * We always support it (via ESC[K), so define it as a non-NULL string.
 */
#define clr_eol "\033[K"

/*
 * WINDOW structure
 */
typedef struct _win_st {
    int _cury, _curx;       /* current cursor position */
    int _maxy, _maxx;       /* window size */
    int _begy, _begx;       /* window origin on screen */
    chtype *_buf;           /* character buffer [_maxy * _maxx] */
    chtype *_disp;          /* last-displayed buffer (stdscr only, NULL for others) */
    attr_t _attrs;          /* current attributes */
    int _flags;             /* various flags */
    int _clear;             /* force full clear on next refresh */
    int _leaveok;           /* leave cursor where it is */
    int _keypad;            /* keypad mode enabled */
    int _is_sub;            /* 1 if subwindow (don't free buf) */
} WINDOW;

/* _flags bits */
#define _ISPAD    0x01

/*
 * Globals
 */
extern WINDOW *stdscr;
extern WINDOW *curscr;
extern int LINES;
extern int COLS;

/*
 * Initialization / termination
 */
WINDOW *initscr(void);
int endwin(void);
int isendwin(void);

/*
 * Window management
 */
WINDOW *newwin(int nlines, int ncols, int begin_y, int begin_x);
WINDOW *subwin(WINDOW *orig, int nlines, int ncols, int begin_y, int begin_x);
int delwin(WINDOW *win);
int mvwin(WINDOW *win, int y, int x);

/*
 * Output — stdscr convenience macros
 */
int wmove(WINDOW *win, int y, int x);
int waddch(WINDOW *win, chtype ch);
int waddstr(WINDOW *win, const char *str);
int wclrtoeol(WINDOW *win);
int wclear(WINDOW *win);
int werase(WINDOW *win);
int wrefresh(WINDOW *win);
int wprintw(WINDOW *win, const char *fmt, ...);
int mvwaddch(WINDOW *win, int y, int x, chtype ch);
int mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...);
chtype mvwinch(WINDOW *win, int y, int x);
int wgetnstr(WINDOW *win, char *str, int n);
int wstandout(WINDOW *win);
int wstandend(WINDOW *win);

#define move(y,x)          wmove(stdscr,(y),(x))
#define addch(ch)          waddch(stdscr,(ch))
#define addstr(s)          waddstr(stdscr,(s))
#define mvaddch(y,x,ch)    (wmove(stdscr,(y),(x)), waddch(stdscr,(ch)))
#define mvaddstr(y,x,s)    (wmove(stdscr,(y),(x)), waddstr(stdscr,(s)))
#define clrtoeol()         wclrtoeol(stdscr)
#define clear()            wclear(stdscr)
#define erase()            werase(stdscr)
#define refresh()          wrefresh(stdscr)
#define standout()         wstandout(stdscr)
#define standend()         wstandend(stdscr)
#define inch()             ((stdscr)->_buf[(stdscr)->_cury * (stdscr)->_maxx + (stdscr)->_curx])
#define mvinch(y,x)        (wmove(stdscr,(y),(x)), inch())
#define printw             ppap_printw
#define mvprintw           ppap_mvprintw

int ppap_printw(const char *fmt, ...);
int ppap_mvprintw(int y, int x, const char *fmt, ...);

/*
 * Input
 */
int getch(void);

/*
 * Mode control
 */
int raw(void);
int noecho(void);
int keypad(WINDOW *win, int bf);
int nocbreak(void);
int halfdelay(int tenths);

/*
 * Window flags
 */
int clearok(WINDOW *win, int bf);
int idlok(WINDOW *win, int bf);
int leaveok(WINDOW *win, int bf);
int touchwin(WINDOW *win);

/*
 * Cursor
 */
int mvcur(int oldrow, int oldcol, int newrow, int newcol);

/*
 * Query
 */
#define getyx(w,y,x)    do { (y) = (w)->_cury; (x) = (w)->_curx; } while(0)
#define getmaxy(w)      ((w)->_maxy)
#define getmaxx(w)      ((w)->_maxx)

/*
 * Misc
 */
char *unctrl(chtype ch);
int erasechar(void);
int killchar(void);
int baudrate(void);
int flushinp(void);

#endif /* PPAP_CURSES_H */