/*
 * ls.c — list directory contents
 *
 * Usage: ls [-l] [path]
 * Default: list names, one per line.
 * -l: show type, permissions, size, name.
 */

#include "lib/uclib.h"

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
  /* Right-align in 8 columns. */
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

int main(int argc, char *argv[]) {
  int long_fmt = 0;
  const char *path = ".";
  int argi = 1;

  while (argi < argc && argv[argi][0] == '-') {
    if (uc_strcmp(argv[argi], "-l") == 0)
      long_fmt = 1;
    else {
      uc_eputs("ls: unknown option: ");
      uc_eputs(argv[argi]);
      uc_eputs("\n");
      return 1;
    }
    argi++;
  }
  if (argi < argc) path = argv[argi];

  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    uc_eputs("ls: cannot open ");
    uc_eputs(path);
    uc_eputs("\n");
    return 1;
  }

  struct dirent de;
  while (getdents(fd, &de, 1) > 0) {
    if (long_fmt) {
      /* Build full path for stat. */
      char fullpath[128];
      int plen = uc_strlen(path);
      if (plen + 1 + uc_strlen(de.d_name) + 1 > (int)sizeof(fullpath)) {
        uc_puts(de.d_name);
        uc_puts("\n");
        continue;
      }
      uc_strcpy(fullpath, path);
      if (plen > 0 && fullpath[plen - 1] != '/') fullpath[plen++] = '/';
      uc_strcpy(fullpath + plen, de.d_name);

      struct stat st;
      if (stat(fullpath, &st) == 0) {
        print_mode(st.st_mode);
        uc_puts(" ");
        print_size(st.st_size);
        uc_puts(" ");
      }
    }
    uc_puts(de.d_name);
    uc_puts("\n");
  }

  close(fd);
  return 0;
}
