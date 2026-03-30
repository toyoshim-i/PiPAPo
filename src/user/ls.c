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

#define TIOCGWINSZ 0x5413u
#define NAME_MAX_STORE 1024 /* name storage pool */
#define ENTRY_MAX 64        /* max entries per directory */

struct winsize {
  unsigned short ws_row;
  unsigned short ws_col;
};

static int opt_long;
static int opt_all;
static int opt_classify;
static int term_cols;

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

static void print_name_classified(const char *name, const struct stat *st,
                                  int have_stat) {
  uc_puts(name);
  if (opt_classify && have_stat) {
    if (S_ISDIR(st->st_mode))
      uc_putc('/');
    else if (st->st_mode & 0111)
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
      int have_stat = 0;
      if (opt_long || opt_classify)
        have_stat = (stat_entry(path, de.d_name, &st) == 0);

      if (opt_long && have_stat) {
        print_mode(st.st_mode);
        uc_putc(' ');
        print_size(st.st_size);
        uc_putc(' ');
      }
      print_name_classified(de.d_name, &st, have_stat);
      uc_putc('\n');
    }
    close(fd);
    return 0;
  }

  /* Multi-column: collect names, find max width, print in columns. */
  static char name_pool[NAME_MAX_STORE];
  static uint16_t name_off[ENTRY_MAX]; /* offsets into name_pool */
  static uint8_t name_len[ENTRY_MAX];  /* display lengths */
  int pool_used = 0;
  int count = 0;
  int max_len = 0;

  struct dirent de;
  while (getdents(fd, &de, sizeof(de)) > 0 && count < ENTRY_MAX) {
    if (!opt_all && de.d_name[0] == '.') continue;

    int nlen = uc_strlen(de.d_name);
    char suffix = 0;
    if (opt_classify) {
      struct stat st;
      if (stat_entry(path, de.d_name, &st) == 0) {
        if (S_ISDIR(st.st_mode))
          suffix = '/';
        else if (st.st_mode & 0111)
          suffix = '*';
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
    if (dlen > max_len) max_len = dlen;
    count++;
  }
  close(fd);

  if (count == 0) return 0;

  int col_width = max_len + 2; /* 2-space gap */
  int ncols = term_cols / col_width;
  if (ncols < 1) ncols = 1;

  for (int i = 0; i < count; i++) {
    uc_puts(name_pool + name_off[i]);
    if (ncols <= 1 || (i + 1) % ncols == 0 || i + 1 == count) {
      uc_putc('\n');
    } else {
      int pad = col_width - name_len[i];
      for (int j = 0; j < pad; j++) uc_putc(' ');
    }
  }

  return 0;
}

int main(int argc, char *argv[]) {
  int argi = 1;

  /* Detect tty for multi-column default. */
  struct winsize ws;
  if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) term_cols = ws.ws_col;

  while (argi < argc && argv[argi][0] == '-') {
    if (uc_strcmp(argv[argi], "--help") == 0) {
      uc_puts(
          "Usage: ls [-laF] [path ...]\n"
          "  -l  Long format (mode, size, name)\n"
          "  -a  Include hidden entries (.*)\n"
          "  -F  Append / for dirs, * for executables\n");
      return 0;
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
    rc = ls_dir(".");
  } else if (argi + 1 == argc) {
    rc = ls_dir(argv[argi]);
  } else {
    for (int i = argi; i < argc; i++) {
      if (i > argi) uc_putc('\n');
      uc_puts(argv[i]);
      uc_puts(":\n");
      if (ls_dir(argv[i])) rc = 1;
    }
  }
  return rc;
}
