/*
 * ls.c — list directory contents
 *
 * Usage: ls [-laF] [path ...]
 * -l: long format (type, permissions, size, name).
 * -a: include entries starting with '.'.
 * -F: append indicator (/ for dirs, * for executables).
 * Default: multi-column if stdout is a tty, one-per-line otherwise.
 */

#include "lib/uclib.h"

#include "common/termios.h"

#define NAME_MAX_STORE 1024 /* name storage pool */
#define ENTRY_MAX 64        /* max entries per directory */

static int opt_long;
static int opt_all;
static int opt_classify;
static int opt_recursive;
static int term_cols;
static int use_color = 1;
#define C(seq) (use_color ? (seq) : "")
#define C_RST     C("\033[0m")
#define C_BOLD    C("\033[1m")
#define C_CYAN    C("\033[36m")
#define C_WHITE   C("\033[37m")
#define C_BBLUE   C("\033[1;34m")
#define C_BGREEN  C("\033[1;32m")
#define C_BCYAN   C("\033[1;36m")
#define C_BYELLOW C("\033[1;33m")
#define C_BMAGENTA C("\033[1;35m")
#define C_DIM     C("\033[2m")

static void print_mode(uint32_t mode) {
  char buf[11];
  buf[0] = S_ISDIR(mode)   ? 'd'
           : S_ISLNK(mode) ? 'l'
           : S_ISCHR(mode) ? 'c'
                           : '-';
  buf[1] = (mode & 0400) ? 'r' : '-';
  buf[2] = (mode & 0200) ? 'w' : '-';
  buf[3] = (mode & 0100) ? 'x' : '-';
  buf[4] = (mode & 040) ? 'r' : '-';
  buf[5] = (mode & 020) ? 'w' : '-';
  buf[6] = (mode & 010) ? 'x' : '-';
  buf[7] = (mode & 04) ? 'r' : '-';
  buf[8] = (mode & 02) ? 'w' : '-';
  buf[9] = (mode & 01) ? 'x' : '-';
  buf[10] = '\0';
  uc_puts(buf);
}

static void print_symlink_mode(void) {
  uc_puts("lrwxrwxrwx");
}

static void print_size(uint32_t size) {
  char buf[9];
  int pos = 8;
  buf[pos] = '\0';
  if (size == 0) {
    buf[--pos] = '0';
  } else {
    while (size && pos > 0) {
      buf[--pos] = (char)('0' + size % 10);
      size /= 10;
    }
  }
  while (pos > 0) buf[--pos] = ' ';
  uc_puts(buf);
}

static int stat_entry(const char *dir, const char *name, struct stat *st) {
  char fullpath[128];
  int dlen = uc_strlen(dir);
  int nlen = uc_strlen(name);
  if (dlen + 1 + nlen + 1 > (int)sizeof(fullpath)) return -1;
  uc_strcpy(fullpath, dir);
  if (dlen > 0 && fullpath[dlen - 1] != '/') fullpath[dlen++] = '/';
  uc_strcpy(fullpath + dlen, name);
  return stat(fullpath, st);
}

static int readlink_entry(const char *dir, const char *name, char *buf,
                          int bufsiz) {
  char fullpath[128];
  int dlen = uc_strlen(dir);
  int nlen = uc_strlen(name);
  if (dlen + 1 + nlen + 1 > (int)sizeof(fullpath)) return -1;
  uc_strcpy(fullpath, dir);
  if (dlen > 0 && fullpath[dlen - 1] != '/') fullpath[dlen++] = '/';
  uc_strcpy(fullpath + dlen, name);
  int n = (int)readlink(fullpath, buf, (size_t)(bufsiz - 1));
  if (n < 0) return -1;
  if (n >= bufsiz) n = bufsiz - 1;
  buf[n] = '\0';
  return n;
}

/* File type codes for coloring: 0=regular, 1=dir, 2=exe, 3=symlink, 4=device */
static void color_for_type(uint8_t ftype) {
  switch (ftype) {
    case 1: uc_puts(C_BBLUE); break;
    case 2: uc_puts(C_BGREEN); break;
    case 3: uc_puts(C_BCYAN); break;
    case 4: uc_puts(C_BYELLOW); break;
  }
}

static uint8_t stat_to_type(const struct stat *st) {
  if (S_ISDIR(st->st_mode)) return 1;
  if (S_ISLNK(st->st_mode)) return 3;
  if (S_ISCHR(st->st_mode)) return 4;
  if (st->st_mode & 0111) return 2;
  return 0;
}

static void print_name_classified(const char *name, const struct stat *st,
                                  int have_stat) {
  uint8_t ftype = have_stat ? stat_to_type(st) : 0;
  if (ftype) color_for_type(ftype);
  uc_puts(name);
  if (ftype) uc_puts(C_RST);
  if (opt_classify && have_stat) {
    if (ftype == 1)
      uc_putc('/');
    else if (ftype == 2)
      uc_putc('*');
  }
}

static int ls_dir(const char *path) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    uc_eputs("ls: cannot open ");
    uc_eputs(path);
    uc_eputs("\n");
    return 1;
  }

  /* For long format or no multi-col, print directly. */
  if (opt_long || term_cols <= 0) {
    struct dirent de;
    while (getdents(fd, &de, sizeof(de)) > 0) {
      if (!opt_all && de.d_name[0] == '.') continue;

      struct stat st;
      int have_stat = (stat_entry(path, de.d_name, &st) == 0);
      char link_target[128];
      int link_len = readlink_entry(path, de.d_name, link_target,
                                    (int)sizeof(link_target));
      int is_symlink = (link_len >= 0);

      if (opt_long && (have_stat || is_symlink)) {
        uc_puts(C_CYAN);
        if (is_symlink)
          print_symlink_mode();
        else
          print_mode(st.st_mode);
        uc_puts(C_RST);
        uc_putc(' ');
        uc_puts(C_WHITE);
        if (is_symlink)
          print_size((uint32_t)link_len);
        else
          print_size(st.st_size);
        uc_puts(C_RST);
        uc_putc(' ');
        /* mtime column — always present in long format.  Symlinks have
         * no stat of their own, so borrow the link target's stat if we
         * have it; otherwise the field is "--". */
        if (have_stat) {
          uc_puts(C_DIM);
          char tbuf[17];
          uc_format_ymdhm(tbuf, st.st_mtime);
          uc_puts(tbuf);
          uc_puts(C_RST);
        } else {
          uc_puts("                ");
        }
        uc_putc(' ');
      }
      if (is_symlink) {
        uc_puts(C_BCYAN);
        uc_puts(de.d_name);
        uc_puts(C_RST);
      } else {
        print_name_classified(de.d_name, &st, have_stat);
      }
      if (opt_long && is_symlink) {
        uc_puts(C_DIM);
        uc_puts(" -> ");
        uc_puts(C_RST);
        uc_puts(C_BMAGENTA);
        uc_puts(link_target);
        uc_puts(C_RST);
      }
      uc_putc('\n');
    }
    close(fd);
    return 0;
  }

  /* Multi-column: collect names, find max width, print in columns. */
  static char name_pool[NAME_MAX_STORE];
  static uint16_t name_off[ENTRY_MAX]; /* offsets into name_pool */
  static uint8_t name_len[ENTRY_MAX];  /* display lengths */
  static uint8_t name_type[ENTRY_MAX]; /* 0=file, 1=dir, 2=exe */
  int pool_used = 0;
  int count = 0;
  int max_len = 0;

  struct dirent de;
  while (getdents(fd, &de, sizeof(de)) > 0 && count < ENTRY_MAX) {
    if (!opt_all && de.d_name[0] == '.') continue;

    int nlen = uc_strlen(de.d_name);
    char suffix = 0;
    uint8_t ftype = 0;
    char link_target[128];
    int is_symlink =
        (readlink_entry(path, de.d_name, link_target, (int)sizeof(link_target)) >=
         0);
    {
      struct stat st;
      if (stat_entry(path, de.d_name, &st) == 0) {
        if (is_symlink)
          ftype = 3;
        else
          ftype = stat_to_type(&st);
        if (opt_classify) {
          if (ftype == 1) suffix = '/';
          else if (ftype == 2) suffix = '*';
          else if (ftype == 3) suffix = '@';
        }
      }
    }
    int dlen = nlen + (suffix ? 1 : 0);

    if (pool_used + dlen + 1 > NAME_MAX_STORE) break;
    name_off[count] = (uint16_t)pool_used;
    uc_memcpy(name_pool + pool_used, de.d_name, nlen);
    pool_used += nlen;
    if (suffix) name_pool[pool_used++] = suffix;
    name_pool[pool_used++] = '\0';
    name_len[count] = (uint8_t)dlen;
    name_type[count] = ftype;
    if (dlen > max_len) max_len = dlen;
    count++;
  }
  close(fd);

  if (count == 0) return 0;

  int col_width = max_len + 2; /* 2-space gap */
  int ncols = term_cols / col_width;
  if (ncols < 1) ncols = 1;

  for (int i = 0; i < count; i++) {
    if (name_type[i]) color_for_type(name_type[i]);
    uc_puts(name_pool + name_off[i]);
    if (name_type[i]) uc_puts(C_RST);
    if (ncols <= 1 || (i + 1) % ncols == 0 || i + 1 == count) {
      uc_putc('\n');
    } else {
      int pad = col_width - name_len[i];
      for (int j = 0; j < pad; j++) uc_putc(' ');
    }
  }

  return 0;
}

/* List `path`, and with -R descend into each subdirectory with a
 * "subpath:" header on a fresh line (GNU-style). */
static int ls_walk(const char *path) {
  int rc = ls_dir(path);
  if (!opt_recursive) return rc;

  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) return rc;
  struct dirent de;
  while (getdents(fd, &de, sizeof(de)) > 0) {
    /* Skip "." and ".." */
    if (de.d_name[0] == '.' &&
        (de.d_name[1] == '\0' ||
         (de.d_name[1] == '.' && de.d_name[2] == '\0')))
      continue;
    if (!opt_all && de.d_name[0] == '.') continue;

    char child[128];
    int dlen = uc_strlen(path);
    int nlen = uc_strlen(de.d_name);
    if (dlen + 1 + nlen + 1 > (int)sizeof(child)) continue;
    uc_strcpy(child, path);
    if (dlen > 0 && child[dlen - 1] != '/') child[dlen++] = '/';
    uc_strcpy(child + dlen, de.d_name);

    struct stat st;
    if (stat(child, &st) != 0) continue;
    if (!S_ISDIR(st.st_mode)) continue;

    uc_putc('\n');
    uc_puts(child);
    uc_puts(":\n");
    if (ls_walk(child)) rc = 1;
  }
  close(fd);
  return rc;
}

int main(int argc, char *argv[]) {
  int argi = 1;

  /* Detect tty for multi-column default. */
  struct winsize ws;
  if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) term_cols = ws.ws_col;

  while (argi < argc && argv[argi][0] == '-') {
    if (uc_strcmp(argv[argi], "--help") == 0) {
      uc_puts(
          "Usage: ls [-laFR] [--no-color] [path ...]\n"
          "  -l  Long format (mode, size, name)\n"
          "  -a  Include hidden entries (.*)\n"
          "  -F  Append / for dirs, * for executables\n"
          "  -R  Recursively list subdirectories\n"
          "  --no-color  Disable color output\n");
      return 0;
    }
    if (uc_strcmp(argv[argi], "--no-color") == 0) {
      use_color = 0;
      argi++;
      continue;
    }
    const char *p = argv[argi] + 1;
    while (*p) {
      switch (*p) {
        case 'l':
          opt_long = 1;
          break;
        case 'a':
          opt_all = 1;
          break;
        case 'F':
          opt_classify = 1;
          break;
        case 'R':
          opt_recursive = 1;
          break;
        default:
          uc_eputs("ls: unknown option: -");
          uc_putc(*p);
          uc_eputs("\n");
          return 1;
      }
      p++;
    }
    argi++;
  }

  int rc = 0;
  if (argi >= argc) {
    rc = ls_walk(".");
  } else if (argi + 1 == argc) {
    rc = ls_walk(argv[argi]);
  } else {
    for (int i = argi; i < argc; i++) {
      if (i > argi) uc_putc('\n');
      uc_puts(argv[i]);
      uc_puts(":\n");
      if (ls_walk(argv[i])) rc = 1;
    }
  }
  return rc;
}
