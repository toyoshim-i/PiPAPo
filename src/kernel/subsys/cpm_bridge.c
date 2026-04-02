/*
 * cpm_bridge.c — CP/M 2.2 BDOS/BIOS bridge (personality layer)
 *
 * Phase 1: BDOS fn 0, 2, 9 (output + exit)
 * Phase 2: BDOS fn 1, 3–8, 10–12 (console I/O) + BIOS console
 * Phase 3: BDOS fn 13–16, 19–23, 26, 33–36, 40 (file operations)
 *
 * See docs/subsystems/cpm.md §5 for the full design.
 */

#include "cpm_bridge.h"

#include <string.h>

#include "common/ptrace.h"
#include "kernel/common/errno.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/mm/mem_region.h"
#include "kernel/proc/proc.h"

/* ── ADM-3A → VT100 escape sequence translator ────────────────────────── */

static long cpm_fd_desc(long fd) {
  if (fd < 0 || (uint32_t)fd >= FD_MAX) return -(long)EBADF;
  if (current->fd_map[(uint32_t)fd] == FD_DESC_NONE) return -(long)EBADF;
  return current->fd_map[(uint32_t)fd];
}

static int cpm_page_ref(const void *buf, page_id_t *page, uint16_t *off) {
  return (mem_region_ptr_ref(buf, page, off) < 0) ? -EFAULT : 0;
}

static long cpm_fd_read(long fd, void *buf, size_t n) {
  long desc = cpm_fd_desc(fd);
  page_id_t page;
  uint16_t off;
  int rc;

  if (desc < 0) return desc;
  rc = cpm_page_ref(buf, &page, &off);
  if (rc < 0) return rc;
  return mod_vfs.fd_read((int)desc, page, off, n);
}

static long cpm_fd_write(long fd, const void *buf, size_t n) {
  long desc = cpm_fd_desc(fd);
  page_id_t page;
  uint16_t off;
  int rc;

  if (desc < 0) return desc;
  rc = cpm_page_ref(buf, &page, &off);
  if (rc < 0) return rc;
  return mod_vfs.fd_write((int)desc, page, off, n);
}

static int cpm_fd_poll(long fd) {
  long desc = cpm_fd_desc(fd);
  if (desc < 0) return (int)desc;
  return mod_vfs.fd_poll((int)desc);
}

static int cpm_fd_ioctl(long fd, uint32_t cmd, void *arg) {
  long desc = cpm_fd_desc(fd);
  if (desc < 0) return (int)desc;
  return mod_vfs.fd_ioctl((int)desc, cmd, arg);
}

/*
 * CP/M programs commonly emit ADM-3A (Lear Siegler) escape sequences.
 * Our fbcon implements VT100/ANSI CSI.  This translator sits in the
 * console output path and converts ADM-3A sequences to VT100 on the fly.
 *
 * Supported ADM-3A sequences:
 *   ESC =  row col   → ESC [ row ; col H   (cursor address, +32 encoding)
 *   ESC *             → ESC [ 2 J           (clear screen)
 *   ESC +             → ESC [ J             (clear to end of screen)
 *   ESC T             → ESC [ K             (clear to end of line)
 *   ESC E             → ESC [ K             (clear to end of line, alias)
 *   ESC )             → (ignored)           (start alternate keypad)
 *   0x1A (Ctrl-Z)     → ESC [ 2 J ESC [ H  (clear screen + home)
 *   0x0B (Ctrl-K)     → ESC [ A             (cursor up)
 *   0x0C (Ctrl-L)     → ESC [ C             (cursor right)
 *   0x1E (Ctrl-^)     → ESC [ H             (cursor home)
 *
 * Supported VT52 sequences:
 *   ESC A             → ESC [ A             (cursor up)
 *   ESC B             → ESC [ B             (cursor down)
 *   ESC C             → ESC [ C             (cursor right)
 *   ESC D             → ESC [ D             (cursor left)
 *   ESC H             → ESC [ H             (cursor home)
 *   ESC I             → ESC M               (reverse line feed)
 *   ESC J             → ESC [ J             (erase to end of screen)
 *   ESC K             → ESC [ K             (erase to end of line)
 *   ESC Y  row col    → ESC [ row ; col H   (cursor address, +32 encoding)
 *
 * Kaypro attribute sequences:
 *   ESC G <n>         → SGR attribute set (bitmask)
 *   ESC B <n>         → SGR attribute on  (Kaypro only)
 *   ESC C <n>         → SGR attribute off (Kaypro only)
 *
 * ESC B and ESC C are ambiguous: VT52 uses them for cursor down/right
 * (no parameter), Kaypro uses them for attribute on/off (followed by a
 * digit).  We auto-detect the dialect from other sequences:
 *   - VT52-only sequences (ESC A/D/H/I/J/K/Y) → latch VT52 mode
 *   - Kaypro-only sequences (ESC G)            → latch Kaypro mode
 * ESC B/C then follow the latched dialect.  Default is VT52.
 *
 * Characters without special meaning pass through unchanged.
 */

enum {
  ADM_NORMAL,
  ADM_ESC,
  ADM_ROW,
  ADM_COL,
  ADM_ATTR_SET,
  ADM_ATTR_ON,
  ADM_ATTR_OFF
};
static int adm_state;
static uint8_t adm_row;

/* Auto-detect VT52 vs Kaypro for ambiguous ESC B / ESC C */
enum { TERM_PASSTHROUGH, TERM_VT52, TERM_KAYPRO };
static int term_type;

/* Per-process terminal type mirror — updated alongside term_type so that
 * procfs can read it from the cpm_state_t without touching global state. */
static cpm_state_t *adm_cpm_ctx;

static void (*adm_raw_out)(uint8_t ch);

static void adm_set_term_type(int dialect) {
  term_type = dialect;
  if (adm_cpm_ctx) adm_cpm_ctx->term_dialect = (uint8_t)dialect;
}

static void adm_emit(const char *s) {
  while (*s) adm_raw_out((uint8_t)*s++);
}

/* Emit a decimal number (1–3 digits, no leading zeros).
 * Use unsigned to avoid __divsi3/__modsi3 on m68k (no libgcc). */
static void adm_emit_num(unsigned n) {
  if (n >= 100) adm_raw_out((uint8_t)('0' + n / 100));
  if (n >= 10) adm_raw_out((uint8_t)('0' + (n / 10) % 10));
  adm_raw_out((uint8_t)('0' + n % 10));
}

static void adm_putchar(uint8_t ch) {
  switch (adm_state) {
    case ADM_NORMAL:
      if (ch == 0x1B) {
        adm_state = ADM_ESC;
      } else if (ch == 0x1A) {
        /* Ctrl-Z: clear screen + home */
        adm_emit("\033[2J\033[H");
      } else if (ch == 0x0B) {
        /* Ctrl-K: cursor up */
        adm_emit("\033[A");
      } else if (ch == 0x0C) {
        /* Ctrl-L: cursor right */
        adm_emit("\033[C");
      } else if (ch == 0x1E) {
        /* Ctrl-^: cursor home */
        adm_emit("\033[H");
      } else if (ch >= 0x20 || ch == '\r' || ch == '\n' || ch == '\t' ||
                 ch == '\b' || ch == '\a') {
        adm_raw_out(ch);
      }
      /* Other control chars (0x01–0x06, 0x0E–0x19, 0x1D, 0x1F)
       * are silently dropped — these are typically WordStar
       * attribute toggles (^A bold, ^B bold, ^S underline, etc.)
       * that have no VT100 equivalent. */
      break;

    case ADM_ESC:
      if (ch == '=') {
        /* ADM-3A ESC = row col */
        adm_state = ADM_ROW;
      } else if (ch == 'Y') {
        /* VT52 ESC Y row col */
        adm_set_term_type(TERM_VT52);
        adm_state = ADM_ROW;
      } else if (ch == '*') {
        adm_emit("\033[2J");
        adm_state = ADM_NORMAL;
      } else if (ch == '+') {
        adm_emit("\033[J");
        adm_state = ADM_NORMAL;
      } else if (ch == 'T' || ch == 'E') {
        adm_emit("\033[K");
        adm_state = ADM_NORMAL;
      } else if (ch == 'A') {
        /* VT52: cursor up */
        adm_set_term_type(TERM_VT52);
        adm_emit("\033[A");
        adm_state = ADM_NORMAL;
      } else if (ch == 'B') {
        /* VT52: cursor down  OR  Kaypro: attribute on */
        if (term_type == TERM_KAYPRO) {
          adm_state = ADM_ATTR_ON;
        } else {
          adm_emit("\033[B");
          adm_state = ADM_NORMAL;
        }
      } else if (ch == 'C') {
        /* VT52: cursor right  OR  Kaypro: attribute off */
        if (term_type == TERM_KAYPRO) {
          adm_state = ADM_ATTR_OFF;
        } else {
          adm_emit("\033[C");
          adm_state = ADM_NORMAL;
        }
      } else if (ch == 'D') {
        /* VT52: cursor left */
        adm_set_term_type(TERM_VT52);
        adm_emit("\033[D");
        adm_state = ADM_NORMAL;
      } else if (ch == 'H') {
        /* VT52: cursor home */
        adm_set_term_type(TERM_VT52);
        adm_emit("\033[H");
        adm_state = ADM_NORMAL;
      } else if (ch == 'I') {
        /* VT52: reverse line feed → VT100 reverse index */
        adm_set_term_type(TERM_VT52);
        adm_emit("\033M");
        adm_state = ADM_NORMAL;
      } else if (ch == 'J') {
        /* VT52: erase to end of screen */
        adm_set_term_type(TERM_VT52);
        adm_emit("\033[J");
        adm_state = ADM_NORMAL;
      } else if (ch == 'K') {
        /* VT52: erase to end of line */
        adm_set_term_type(TERM_VT52);
        adm_emit("\033[K");
        adm_state = ADM_NORMAL;
      } else if (ch == ')') {
        /* Alternate keypad mode — ignore */
        adm_state = ADM_NORMAL;
      } else if (ch == 'G') {
        /* Kaypro ESC G <n> — set attribute (bitmask):
         *   0=normal, 1=underline, 2=blink, 4=bold/half, 8=reverse */
        adm_set_term_type(TERM_KAYPRO);
        adm_state = ADM_ATTR_SET;
      } else if (ch == '[') {
        /* Already a VT100 CSI — pass through as-is */
        adm_raw_out(0x1B);
        adm_raw_out('[');
        adm_state = ADM_NORMAL;
      } else if (ch >= 0x20) {
        /* Unknown ESC sequence with printable char — pass through */
        adm_raw_out(0x1B);
        adm_raw_out(ch);
        adm_state = ADM_NORMAL;
      } else {
        /* ESC + control char (e.g. ESC ^A for WordStar attributes)
         * — silently drop both bytes */
        adm_state = ADM_NORMAL;
      }
      break;

    case ADM_ROW:
      adm_row = ch;
      adm_state = ADM_COL;
      break;

    case ADM_COL:
      /* ADM-3A: row and col are encoded as value + 0x20 (space) */
      adm_emit("\033[");
      adm_emit_num((adm_row - 0x20) + 1); /* 1-based */
      adm_raw_out(';');
      adm_emit_num((ch - 0x20) + 1); /* 1-based */
      adm_raw_out('H');
      adm_state = ADM_NORMAL;
      break;

    case ADM_ATTR_SET:
      /* ESC G <n>: Kaypro set-attribute (bitmask).
       * Map to VT100 SGR: reset first, then enable bits. */
      adm_emit("\033[0");
      if (ch & 0x01) adm_emit(";4"); /* underline */
      if (ch & 0x02) adm_emit(";5"); /* blink */
      if (ch & 0x04) adm_emit(";1"); /* bold */
      if (ch & 0x08) adm_emit(";7"); /* reverse */
      adm_raw_out('m');
      adm_state = ADM_NORMAL;
      break;

    case ADM_ATTR_ON:
      /* Kaypro ESC B <n>: start individual attribute */
      switch (ch) {
        case '0':
          adm_emit("\033[2m");
          break; /* dim */
        case '1':
          adm_emit("\033[4m");
          break; /* underline */
        case '2':
          adm_emit("\033[5m");
          break; /* blink */
        case '3':
          adm_emit("\033[4m");
          break; /* underline (alias) */
        case '4':
          adm_emit("\033[1m");
          break; /* bold */
        default:
          break;
      }
      adm_state = ADM_NORMAL;
      break;

    case ADM_ATTR_OFF:
      /* Kaypro ESC C <n>: end individual attribute */
      switch (ch) {
        case '0':
          adm_emit("\033[22m");
          break; /* dim off */
        case '1':
          adm_emit("\033[24m");
          break; /* underline off */
        case '2':
          adm_emit("\033[25m");
          break; /* blink off */
        case '3':
          adm_emit("\033[24m");
          break; /* underline off */
        case '4':
          adm_emit("\033[22m");
          break; /* bold off */
        default:
          break;
      }
      adm_state = ADM_NORMAL;
      break;
  }
}

/* ── Platform I/O abstraction ──────────────────────────────────────────── */

#ifdef PPAP_KERNEL

#include "common/poll.h"
#include "kernel/fd/fd.h"
#include "kernel/signal/signal.h"
#include "kernel/syscall/syscall.h"

static int cpm_trace_ret(const z80_state_t *cpu) {
  return (int)(((uint32_t)cpu->a << 16) | z80_hl(cpu));
}

static void cpm_trace_before(uint32_t abi, uint32_t nr, z80_state_t *cpu) {
  (void)trace_before_subsys(abi, nr, z80_af(cpu), z80_bc(cpu), z80_de(cpu),
                            z80_hl(cpu), cpu->sp, cpu->pc);
}

static void cpm_trace_after(uint32_t abi, uint32_t nr, z80_state_t *cpu) {
  trace_after_subsys(abi, nr, z80_af(cpu), z80_bc(cpu), z80_de(cpu),
                     z80_hl(cpu), cpu->sp, cpu->pc, cpm_trace_ret(cpu));
}

static void cpm_raw_putchar(uint8_t ch) { cpm_fd_write(1, &ch, 1); }

static void cpm_putchar(uint8_t ch) {
  adm_raw_out = cpm_raw_putchar;
  adm_putchar(ch);
}

static uint8_t cpm_getchar(void) {
  uint8_t ch = 0;
  for (;;) {
    long rc = cpm_fd_read(0, &ch, 1);
    if (rc > 0) return ch;
    /* tty_read() blocks by setting PROC_BLOCKED + sched_switch() and
     * returns 0 when the task is resumed.  Kernel-mode callers do not
     * get the user-space SVC restart path, so retry until a byte arrives
     * or a pending signal takes the default action. */
    signal_check_kernel();
  }
}

static int cpm_char_ready(void) {
  int mask = cpm_fd_poll(0);
  if (mask < 0) return 0;
  return (mask & POLLIN) ? 1 : 0;
}

static int cpm_file_open(const char *path, int flags) {
  return (int)sys_open((uintptr_t)path, (long)flags, 0644);
}

static int cpm_file_close(int fd) { return (int)sys_close((long)fd); }

static int cpm_file_read(int fd, void *buf, int count) {
  return (int)cpm_fd_read((long)fd, buf, (size_t)count);
}

static int cpm_file_write(int fd, const void *buf, int count) {
  return (int)cpm_fd_write((long)fd, buf, (size_t)count);
}

static int cpm_file_seek(int fd, int offset, int whence) {
  return (int)sys_lseek((long)fd, (long)offset, (long)whence);
}

static int cpm_file_delete(const char *path) {
  return (int)sys_unlink((uintptr_t)path);
}

static int cpm_file_rename(const char *oldpath, const char *newpath) {
  return (int)sys_rename((uintptr_t)oldpath, (uintptr_t)newpath);
}

/* Directory operations — stub for now (PPAP VFS doesn't have opendir yet) */
static void *cpm_dir_open(const char *path) {
  (void)path;
  return NULL;
}

static const char *cpm_dir_read(void *dir) {
  (void)dir;
  return NULL;
}

static void cpm_dir_close(void *dir) { (void)dir; }

#else /* Host test hooks — use POSIX */

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

extern void cpm_host_putchar(uint8_t ch);
extern int cpm_host_getchar(void);
extern int cpm_host_char_ready(void);

static void cpm_putchar(uint8_t ch) {
  adm_raw_out = cpm_host_putchar;
  adm_putchar(ch);
}
static uint8_t cpm_getchar(void) { return (uint8_t)cpm_host_getchar(); }
static int cpm_char_ready(void) { return cpm_host_char_ready(); }

static int cpm_file_open(const char *path, int flags) {
  return open(path, flags, 0644);
}

static int cpm_file_close(int fd) { return close(fd); }
static int cpm_file_read(int fd, void *buf, int count) {
  return (int)read(fd, buf, count);
}

static int cpm_file_write(int fd, const void *buf, int count) {
  return (int)write(fd, buf, count);
}

static int cpm_file_seek(int fd, int offset, int whence) {
  return (int)lseek(fd, offset, whence);
}

static int cpm_file_delete(const char *path) { return unlink(path); }

static int cpm_file_rename(const char *oldpath, const char *newpath) {
  return rename(oldpath, newpath);
}

static void *cpm_dir_open(const char *path) { return opendir(path); }

static const char *cpm_dir_read(void *dir) {
  struct dirent *ent = readdir((DIR *)dir);
  if (!ent) return NULL;
  return ent->d_name;
}

static void cpm_dir_close(void *dir) { closedir((DIR *)dir); }

static void cpm_trace_before(uint32_t abi, uint32_t nr, z80_state_t *cpu) {
  (void)abi;
  (void)nr;
  (void)cpu;
}

static void cpm_trace_after(uint32_t abi, uint32_t nr, z80_state_t *cpu) {
  (void)abi;
  (void)nr;
  (void)cpu;
}

#endif

/* ── O_RDONLY / O_RDWR / O_CREAT / O_TRUNC / SEEK_SET / SEEK_END ──────── */

#ifdef PPAP_KERNEL
/* PPAP defines these in its own headers */
#else
#include <fcntl.h>
#endif

/* ── FCB-to-path translation ───────────────────────────────────────────── */

static int cpm_drive_root_path(const cpm_state_t *cpm, uint8_t drive,
                               char *path, int path_size) {
  if (drive == 0 && cpm->drive_a_root[0]) {
    int n = 0;
    while (cpm->drive_a_root[n] && n < path_size - 1) {
      path[n] = cpm->drive_a_root[n];
      n++;
    }
    path[n] = 0;
    return n;
  }

  if (path_size < 3) return 0;
  path[0] = '/';
  path[1] = 'a' + drive;
  path[2] = 0;
  return 2;
}

void cpm_fcb_to_path(cpm_state_t *cpm, const uint8_t *fcb, char *path,
                     int path_size) {
  /* Determine drive */
  uint8_t drive = fcb[0];
  if (drive == 0) drive = cpm->current_drive + 1; /* default → current */
  drive--;

  /* Extract filename (strip trailing spaces) */
  char name[9], ext[4];
  int nlen = 8, elen = 3;
  memcpy(name, &fcb[1], 8);
  name[8] = 0;
  memcpy(ext, &fcb[9], 3);
  ext[3] = 0;

  /* Strip high bits (CP/M uses bit 7 for attributes) */
  for (int i = 0; i < 8; i++) name[i] &= 0x7F;
  for (int i = 0; i < 3; i++) ext[i] &= 0x7F;

  while (nlen > 0 && name[nlen - 1] == ' ') nlen--;
  while (elen > 0 && ext[elen - 1] == ' ') elen--;
  name[nlen] = 0;
  ext[elen] = 0;

  /* Build path: <drive-root>/NAME.EXT */
  int n = cpm_drive_root_path(cpm, drive, path, path_size);
  if (n > 0 && path[n - 1] != '/' && n < path_size - 1) path[n++] = '/';

  if (elen > 0) {
    for (int i = 0; i < nlen && n < path_size - 5; i++) path[n++] = name[i];
    path[n++] = '.';
    for (int i = 0; i < elen && n < path_size - 1; i++) path[n++] = ext[i];
    path[n] = 0;
  } else {
    for (int i = 0; i < nlen && n < path_size - 1; i++) path[n++] = name[i];
    path[n] = 0;
  }
}

/* ── Wildcard matching ─────────────────────────────────────────────────── */

/*
 * Parse a host filename (e.g. "HELLO.COM") into 8+3 FCB fields.
 * Returns 0 on success, -1 if the name doesn't fit CP/M conventions.
 */
static int cpm_parse_filename(const char *filename, uint8_t *name8,
                              uint8_t *ext3) {
  memset(name8, ' ', 8);
  memset(ext3, ' ', 3);

  int i = 0, pos = 0;

  /* Skip dot and dotdot */
  if (filename[0] == '.') return -1;

  /* Copy name part (up to 8 chars, stop at '.' or end) */
  while (filename[i] && filename[i] != '.' && pos < 8) {
    name8[pos++] = filename[i++];
  }
  if (filename[i] && filename[i] != '.') return -1; /* name too long */

  /* Skip the dot */
  if (filename[i] == '.') {
    i++;
    pos = 0;
    while (filename[i] && pos < 3) {
      ext3[pos++] = filename[i++];
    }
    if (filename[i]) return -1; /* extension too long */
  }

  /* Uppercase the fields */
  for (int j = 0; j < 8; j++)
    if (name8[j] >= 'a' && name8[j] <= 'z') name8[j] -= 32;
  for (int j = 0; j < 3; j++)
    if (ext3[j] >= 'a' && ext3[j] <= 'z') ext3[j] -= 32;

  return 0;
}

/*
 * Match a filename against a CP/M FCB pattern (8+3 fields).
 * '?' in the pattern matches any character.
 * Returns 1 if match, 0 if not.
 */
int cpm_match_fcb(const uint8_t *pattern, const char *filename) {
  uint8_t name8[8], ext3[3];
  if (cpm_parse_filename(filename, name8, ext3) < 0) return 0;

  /* Compare name field (8 bytes) */
  for (int i = 0; i < 8; i++) {
    uint8_t p = pattern[i] & 0x7F;
    if (p != '?' && p != name8[i]) return 0;
  }

  /* Compare extension field (3 bytes) */
  for (int i = 0; i < 3; i++) {
    uint8_t p = pattern[8 + i] & 0x7F;
    if (p != '?' && p != ext3[i]) return 0;
  }

  return 1;
}

/*
 * Build a directory path for a drive.
 */
static void cpm_drive_dir_path(const cpm_state_t *cpm, uint8_t drive,
                               char *path, int path_size) {
  (void)cpm_drive_root_path(cpm, drive, path, path_size);
}

/*
 * Fill a 32-byte DMA directory entry from a filename.
 * Placed at DMA + (slot * 32), slot is always 0 for simplicity.
 */
static void cpm_fill_dir_entry(z80_state_t *cpu, cpm_state_t *cpm,
                               const char *filename, uint8_t user) {
  uint16_t dma = cpm->dma_addr;
  uint8_t name8[8], ext3[3];

  cpm_parse_filename(filename, name8, ext3);

  z80_write8(cpu, dma + 0, user); /* user number */
  for (int i = 0; i < 8; i++) z80_write8(cpu, dma + 1 + i, name8[i]);
  for (int i = 0; i < 3; i++) z80_write8(cpu, dma + 9 + i, ext3[i]);
  /* Extent info + allocation map: zero */
  for (int i = 12; i < 32; i++) z80_write8(cpu, dma + i, 0);
}

/* Forward declaration */
static int cpm_search_next(z80_state_t *cpu, cpm_state_t *cpm);

/*
 * Search First (BDOS function 17)
 * Opens directory, finds first matching file.
 * Returns 0 (slot index in DMA) on match, 0xFF if no match.
 */
static int cpm_search_first(z80_state_t *cpu, cpm_state_t *cpm,
                            uint16_t fcb_addr) {
  /* Close any previous search */
  if (cpm->search_dir) {
    cpm_dir_close(cpm->search_dir);
    cpm->search_dir = NULL;
  }

  /* Read the pattern from the FCB */
  uint8_t fcb_drive = z80_read8(cpu, fcb_addr);
  uint8_t drive = fcb_drive ? (fcb_drive - 1) : cpm->current_drive;
  cpm->search_drive = drive;

  for (int i = 0; i < 11; i++)
    cpm->search_pattern[i] = z80_read8(cpu, fcb_addr + 1 + i);
  cpm->search_pattern[11] = 0;

  /* Open the directory */
  char dir_path[VFS_PATH_MAX];
  cpm_drive_dir_path(cpm, drive, dir_path, sizeof(dir_path));
  cpm->search_dir = cpm_dir_open(dir_path);
  if (!cpm->search_dir) return 0xFF;

  /* Find first match via search_next */
  return cpm_search_next(cpu, cpm);
}

/*
 * Search Next (BDOS function 18)
 * Continues directory search from where Search First left off.
 * Returns 0 on match (entry at DMA+0), 0xFF if no more matches.
 */
static int cpm_search_next(z80_state_t *cpu, cpm_state_t *cpm) {
  if (!cpm->search_dir) return 0xFF;

  const char *name;
  while ((name = cpm_dir_read(cpm->search_dir)) != NULL) {
    if (cpm_match_fcb(cpm->search_pattern, name)) {
      cpm_fill_dir_entry(cpu, cpm, name, cpm->current_user);
      return 0; /* entry at DMA slot 0 */
    }
  }

  /* No more matches — close directory */
  cpm_dir_close(cpm->search_dir);
  cpm->search_dir = NULL;
  return 0xFF;
}

/* ── File slot management ──────────────────────────────────────────────── */

static int cpm_alloc_file_slot(cpm_state_t *cpm, uint16_t fcb_addr, int fd) {
  for (int i = 0; i < CPM_MAX_OPEN_FILES; i++) {
    if (cpm->open_files[i].fcb_addr == 0) {
      cpm->open_files[i].fcb_addr = fcb_addr;
      cpm->open_files[i].fd = fd;
      cpm->open_files[i].file_pos = 0;
      return i;
    }
  }
  return -1;
}

static int cpm_find_file_slot(cpm_state_t *cpm, uint16_t fcb_addr) {
  for (int i = 0; i < CPM_MAX_OPEN_FILES; i++) {
    if (cpm->open_files[i].fcb_addr == fcb_addr) return i;
  }
  return -1;
}

static void cpm_free_file_slot(cpm_state_t *cpm, int slot) {
  cpm->open_files[slot].fcb_addr = 0;
  cpm->open_files[slot].fd = -1;
  cpm->open_files[slot].file_pos = 0;
}

/* ── FCB position helpers ──────────────────────────────────────────────── */

/* Read the sequential position from FCB extent/cr fields.
 * Position = (extent * 128 + current_record) * 128 bytes. */
static uint32_t cpm_fcb_get_seq_pos(z80_state_t *cpu, uint16_t fcb_addr) {
  uint8_t ex = z80_read8(cpu, fcb_addr + 12); /* extent low */
  uint8_t s2 = z80_read8(cpu, fcb_addr + 14); /* extent high */
  uint8_t cr = z80_read8(cpu, fcb_addr + 32); /* current record */
  uint32_t extent = ((uint32_t)s2 << 5) | (ex & 0x1F);
  return (extent * 128 + cr) * CPM_RECORD_SIZE;
}

/* Write back the sequential position to FCB extent/cr fields. */
static void cpm_fcb_set_seq_pos(z80_state_t *cpu, uint16_t fcb_addr,
                                uint32_t byte_pos) {
  uint32_t record = byte_pos / CPM_RECORD_SIZE;
  uint32_t extent = record / 128;
  uint8_t cr = record % 128;

  z80_write8(cpu, fcb_addr + 12, extent & 0x1F);        /* ex */
  z80_write8(cpu, fcb_addr + 14, (extent >> 5) & 0x3F); /* s2 */
  z80_write8(cpu, fcb_addr + 32, cr);                   /* cr */
}

/* ── BDOS Console I/O functions ────────────────────────────────────────── */

static int cpm_console_input(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpu;
  (void)cpm;
  uint8_t ch = cpm_getchar();
  cpm_putchar(ch);
  return ch;
}

static void cpm_console_output(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpm;
  cpm_putchar(cpu->e);
}

static int cpm_reader_input(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpu;
  (void)cpm;
  return cpm_getchar();
}

static void cpm_punch_output(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpm;
  cpm_putchar(cpu->e);
}

static void cpm_list_output(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpm;
  cpm_putchar(cpu->e);
}

static int cpm_direct_console_io(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpm;
  uint8_t e = cpu->e;
  if (e == 0xFF) {
    if (cpm_char_ready()) return cpm_getchar();
    return 0x00;
  } else if (e == 0xFE) {
    return cpm_char_ready() ? 0xFF : 0x00;
  } else {
    cpm_putchar(e);
    return 0;
  }
}

static int cpm_get_iobyte(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpm;
  return z80_read8(cpu, 0x0003);
}

static void cpm_set_iobyte(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpm;
  z80_write8(cpu, 0x0003, cpu->e);
}

static void cpm_print_string(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpm;
  uint16_t addr = z80_de(cpu);
  for (;;) {
    uint8_t ch = z80_read8(cpu, addr);
    if (ch == '$') break;
    cpm_putchar(ch);
    addr++;
  }
}

static void cpm_read_console_buffer(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpm;
  uint16_t buf_addr = z80_de(cpu);
  uint8_t max = z80_read8(cpu, buf_addr);
  int n = 0;

  while (n < max) {
    uint8_t ch = cpm_getchar();
    if (ch == '\r' || ch == '\n') {
      cpm_putchar('\r');
      cpm_putchar('\n');
      break;
    }
    if (ch == 0x08 || ch == 0x7F) {
      if (n > 0) {
        n--;
        cpm_putchar(0x08);
        cpm_putchar(' ');
        cpm_putchar(0x08);
      }
      continue;
    }
    cpm_putchar(ch);
    z80_write8(cpu, buf_addr + 2 + n, ch);
    n++;
  }
  z80_write8(cpu, buf_addr + 1, (uint8_t)n);
}

static int cpm_console_status(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpu;
  (void)cpm;
  return cpm_char_ready() ? 0xFF : 0x00;
}

static int cpm_version_number(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpu;
  (void)cpm;
  return 0x0022;
}

/* ── BDOS Disk/File functions (Phase 3) ────────────────────────────────── */

/*
 * Reset Disk System (BDOS function 13)
 */
static void cpm_reset_disk(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpu;
  cpm->current_drive = 0;
  cpm->dma_addr = CPM_DMA_DEFAULT;
}

/*
 * Select Disk (BDOS function 14)
 * E = drive number (0=A, 1=B, ...)
 */
static void cpm_select_disk(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpu;
  cpm->current_drive = cpu->e & 0x0F;
}

/*
 * Open File (BDOS function 15)
 * DE = FCB address.  Returns A=0 on success, A=0xFF on error.
 */
static int cpm_open_file(z80_state_t *cpu, cpm_state_t *cpm,
                         uint16_t fcb_addr) {
  uint8_t fcb[36];
  for (int i = 0; i < 36; i++) fcb[i] = z80_read8(cpu, fcb_addr + i);

  char path[128];
  cpm_fcb_to_path(cpm, fcb, path, sizeof(path));

  int fd = cpm_file_open(path, O_RDWR);
  if (fd < 0) fd = cpm_file_open(path, O_RDONLY);
  if (fd < 0) return 0xFF;

  int slot = cpm_alloc_file_slot(cpm, fcb_addr, fd);
  if (slot < 0) {
    cpm_file_close(fd);
    return 0xFF;
  }

  /* Initialize FCB sequential position fields */
  z80_write8(cpu, fcb_addr + 12, 0); /* extent low (ex) */
  z80_write8(cpu, fcb_addr + 14, 0); /* extent high (s2) */
  z80_write8(cpu, fcb_addr + 15, 0); /* record count (rc) */
  z80_write8(cpu, fcb_addr + 32, 0); /* current record (cr) */

  return 0;
}

/*
 * Close File (BDOS function 16)
 */
static int cpm_close_file(z80_state_t *cpu, cpm_state_t *cpm,
                          uint16_t fcb_addr) {
  (void)cpu;
  int slot = cpm_find_file_slot(cpm, fcb_addr);
  if (slot < 0) return 0xFF;

  cpm_file_close(cpm->open_files[slot].fd);
  cpm_free_file_slot(cpm, slot);
  return 0;
}

/*
 * Delete File (BDOS function 19)
 */
static int cpm_delete_file(z80_state_t *cpu, cpm_state_t *cpm,
                           uint16_t fcb_addr) {
  uint8_t fcb[36];
  for (int i = 0; i < 36; i++) fcb[i] = z80_read8(cpu, fcb_addr + i);

  char path[128];
  cpm_fcb_to_path(cpm, fcb, path, sizeof(path));

  if (cpm_file_delete(path) < 0) return 0xFF;
  return 0;
}

/*
 * Read Sequential (BDOS function 20)
 * Reads 128 bytes into DMA buffer.  Returns A=0 on success, nonzero on EOF.
 */
static int cpm_read_sequential(z80_state_t *cpu, cpm_state_t *cpm,
                               uint16_t fcb_addr) {
  int slot = cpm_find_file_slot(cpm, fcb_addr);
  if (slot < 0) return 0x09;

  /* Seek to current position */
  uint32_t pos = cpm->open_files[slot].file_pos;
  cpm_file_seek(cpm->open_files[slot].fd, (int)pos, SEEK_SET);

  /* Read 128 bytes */
  uint8_t buf[CPM_RECORD_SIZE];
  int n = cpm_file_read(cpm->open_files[slot].fd, buf, CPM_RECORD_SIZE);
  if (n <= 0) return 0x01; /* EOF */

  /* Pad with 0x1A if short read */
  if (n < CPM_RECORD_SIZE) memset(&buf[n], 0x1A, CPM_RECORD_SIZE - n);

  /* Write to DMA buffer in emulated memory */
  for (int i = 0; i < CPM_RECORD_SIZE; i++)
    z80_write8(cpu, cpm->dma_addr + i, buf[i]);

  /* Advance position */
  cpm->open_files[slot].file_pos += CPM_RECORD_SIZE;
  cpm_fcb_set_seq_pos(cpu, fcb_addr, cpm->open_files[slot].file_pos);

  return 0;
}

/*
 * Write Sequential (BDOS function 21)
 * Writes 128 bytes from DMA buffer.
 */
static int cpm_write_sequential(z80_state_t *cpu, cpm_state_t *cpm,
                                uint16_t fcb_addr) {
  int slot = cpm_find_file_slot(cpm, fcb_addr);
  if (slot < 0) return 0x09;

  /* Seek to current position */
  uint32_t pos = cpm->open_files[slot].file_pos;
  cpm_file_seek(cpm->open_files[slot].fd, (int)pos, SEEK_SET);

  /* Read 128 bytes from DMA buffer */
  uint8_t buf[CPM_RECORD_SIZE];
  for (int i = 0; i < CPM_RECORD_SIZE; i++)
    buf[i] = z80_read8(cpu, cpm->dma_addr + i);

  int n = cpm_file_write(cpm->open_files[slot].fd, buf, CPM_RECORD_SIZE);
  if (n < CPM_RECORD_SIZE) return 0x01; /* disk full */

  /* Advance position */
  cpm->open_files[slot].file_pos += CPM_RECORD_SIZE;
  cpm_fcb_set_seq_pos(cpu, fcb_addr, cpm->open_files[slot].file_pos);

  return 0;
}

/*
 * Make File (BDOS function 22)
 * Creates a new file.  Returns A=0 on success, A=0xFF on error.
 */
static int cpm_make_file(z80_state_t *cpu, cpm_state_t *cpm,
                         uint16_t fcb_addr) {
  uint8_t fcb[36];
  for (int i = 0; i < 36; i++) fcb[i] = z80_read8(cpu, fcb_addr + i);

  char path[128];
  cpm_fcb_to_path(cpm, fcb, path, sizeof(path));

  int fd = cpm_file_open(path, O_RDWR | O_CREAT | O_TRUNC);
  if (fd < 0) return 0xFF;

  int slot = cpm_alloc_file_slot(cpm, fcb_addr, fd);
  if (slot < 0) {
    cpm_file_close(fd);
    return 0xFF;
  }

  z80_write8(cpu, fcb_addr + 12, 0);
  z80_write8(cpu, fcb_addr + 14, 0);
  z80_write8(cpu, fcb_addr + 15, 0);
  z80_write8(cpu, fcb_addr + 32, 0);

  return 0;
}

/*
 * Rename File (BDOS function 23)
 * DE = FCB where bytes 0-15 are the old name and bytes 16-31 are the new name.
 */
static int cpm_rename_file(z80_state_t *cpu, cpm_state_t *cpm,
                           uint16_t fcb_addr) {
  uint8_t old_fcb[16], new_fcb[16];
  for (int i = 0; i < 16; i++) {
    old_fcb[i] = z80_read8(cpu, fcb_addr + i);
    new_fcb[i] = z80_read8(cpu, fcb_addr + 16 + i);
  }

  char old_path[128], new_path[128];
  cpm_fcb_to_path(cpm, old_fcb, old_path, sizeof(old_path));
  cpm_fcb_to_path(cpm, new_fcb, new_path, sizeof(new_path));

  if (cpm_file_rename(old_path, new_path) < 0) return 0xFF;
  return 0;
}

/*
 * Set DMA Address (BDOS function 26)
 */
static void cpm_set_dma_address(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpu;
  cpm->dma_addr = z80_de(cpu);
}

/*
 * Read Random (BDOS function 33)
 * Random record number in FCB bytes 33-35 (little-endian).
 */
static int cpm_read_random(z80_state_t *cpu, cpm_state_t *cpm,
                           uint16_t fcb_addr) {
  int slot = cpm_find_file_slot(cpm, fcb_addr);
  if (slot < 0) return 0x09;

  uint32_t record = z80_read8(cpu, fcb_addr + 33) |
                    ((uint32_t)z80_read8(cpu, fcb_addr + 34) << 8) |
                    ((uint32_t)z80_read8(cpu, fcb_addr + 35) << 16);

  uint32_t offset = record * CPM_RECORD_SIZE;
  cpm->open_files[slot].file_pos = offset;

  /* Update sequential position to match */
  cpm_fcb_set_seq_pos(cpu, fcb_addr, offset);

  return cpm_read_sequential(cpu, cpm, fcb_addr);
}

/*
 * Write Random (BDOS function 34)
 */
static int cpm_write_random(z80_state_t *cpu, cpm_state_t *cpm,
                            uint16_t fcb_addr) {
  int slot = cpm_find_file_slot(cpm, fcb_addr);
  if (slot < 0) return 0x09;

  uint32_t record = z80_read8(cpu, fcb_addr + 33) |
                    ((uint32_t)z80_read8(cpu, fcb_addr + 34) << 8) |
                    ((uint32_t)z80_read8(cpu, fcb_addr + 35) << 16);

  uint32_t offset = record * CPM_RECORD_SIZE;
  cpm->open_files[slot].file_pos = offset;

  cpm_fcb_set_seq_pos(cpu, fcb_addr, offset);

  return cpm_write_sequential(cpu, cpm, fcb_addr);
}

/*
 * Compute File Size (BDOS function 35)
 * Sets random record field in FCB to file size in records.
 */
static int cpm_compute_file_size(z80_state_t *cpu, cpm_state_t *cpm,
                                 uint16_t fcb_addr) {
  int slot = cpm_find_file_slot(cpm, fcb_addr);
  if (slot < 0) return 0xFF;

  int end = cpm_file_seek(cpm->open_files[slot].fd, 0, SEEK_END);
  if (end < 0) return 0xFF;

  uint32_t records = ((uint32_t)end + CPM_RECORD_SIZE - 1) / CPM_RECORD_SIZE;
  z80_write8(cpu, fcb_addr + 33, records & 0xFF);
  z80_write8(cpu, fcb_addr + 34, (records >> 8) & 0xFF);
  z80_write8(cpu, fcb_addr + 35, (records >> 16) & 0xFF);

  return 0;
}

/*
 * Set Random Record (BDOS function 36)
 * Computes random record number from FCB sequential position.
 */
static int cpm_set_random_record(z80_state_t *cpu, cpm_state_t *cpm,
                                 uint16_t fcb_addr) {
  (void)cpm;
  uint32_t pos = cpm_fcb_get_seq_pos(cpu, fcb_addr);
  uint32_t record = pos / CPM_RECORD_SIZE;

  z80_write8(cpu, fcb_addr + 33, record & 0xFF);
  z80_write8(cpu, fcb_addr + 34, (record >> 8) & 0xFF);
  z80_write8(cpu, fcb_addr + 35, (record >> 16) & 0xFF);

  return 0;
}

/*
 * Return Login Vector (BDOS function 24)
 */
static int cpm_return_login_vector(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpu;
  (void)cpm;
  return 0x0001; /* drive A only */
}

/*
 * Return Current Disk (BDOS function 25)
 */
static int cpm_return_current_disk(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpu;
  return cpm->current_drive;
}

/*
 * Get Read-Only Vector (BDOS function 29)
 */
static int cpm_get_readonly_vector(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpu;
  (void)cpm;
  return 0x0000; /* no read-only drives */
}

/*
 * Get Alloc Vector (BDOS function 27)
 * Returns HL pointing to allocation bitmap in Z80 memory.
 * We synthesize a small bitmap at a fixed address (0xFD00) showing
 * a mostly-free disk.  The bitmap format: 1 bit per block, MSB first.
 * We report all blocks as allocated (0xFF bytes) since PPAP doesn't
 * track CP/M disk blocks — this is safe and prevents programs from
 * thinking they can allocate "free" blocks.
 */
#define CPM_ALV_ADDR 0xFD00 /* just below BIOS at 0xFE00 */
#define CPM_ALV_SIZE 32     /* 256 blocks / 8 = 32 bytes */

static int cpm_get_alloc_vector(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpm;
  /* Fill ALV with 0xFF (all allocated) */
  for (int i = 0; i < CPM_ALV_SIZE; i++)
    z80_write8(cpu, CPM_ALV_ADDR + i, 0xFF);
  z80_set_hl(cpu, CPM_ALV_ADDR);
  return 0;
}

/*
 * Get Disk Parameter Block (BDOS function 31)
 * Returns HL pointing to a synthesized DPB in Z80 memory.
 * The DPB describes a standard 8" SSSD floppy (IBM 3740 format):
 *   SPT=26, BSH=3, BLM=7, EXM=0, DSM=242, DRM=63, AL0=0xC0, AL1=0,
 *   CKS=16, OFF=2
 * This is a safe default that works with most CP/M programs.
 */
#define CPM_DPB_ADDR (CPM_ALV_ADDR - 16) /* 0xFCF0 */

static int cpm_get_dpb(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpm;
  uint16_t a = CPM_DPB_ADDR;
  z80_write16(cpu, a + 0, 26);   /* SPT: sectors per track */
  z80_write8(cpu, a + 2, 3);     /* BSH: block shift */
  z80_write8(cpu, a + 3, 7);     /* BLM: block mask */
  z80_write8(cpu, a + 4, 0);     /* EXM: extent mask */
  z80_write16(cpu, a + 5, 242);  /* DSM: max block number */
  z80_write16(cpu, a + 7, 63);   /* DRM: max directory entry */
  z80_write8(cpu, a + 9, 0xC0);  /* AL0: alloc bitmap byte 0 */
  z80_write8(cpu, a + 10, 0x00); /* AL1: alloc bitmap byte 1 */
  z80_write16(cpu, a + 11, 16);  /* CKS: directory check size */
  z80_write16(cpu, a + 13, 2);   /* OFF: reserved tracks */
  z80_set_hl(cpu, CPM_DPB_ADDR);
  return 0;
}

/*
 * Write Protect Disk (BDOS function 28)
 * No-op — PPAP doesn't support write-protecting drives.
 */
static void cpm_write_protect_disk(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpu;
  (void)cpm;
}

/*
 * Set File Attributes (BDOS function 30)
 * No-op — returns success. CP/M file attributes (R/O, SYS) are
 * stored in the high bits of the FCB filename bytes.
 */
static int cpm_set_file_attributes(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpu;
  (void)cpm;
  return 0; /* success */
}

/*
 * Get/Set User Code (BDOS function 32)
 */
static int cpm_set_get_user_code(z80_state_t *cpu, cpm_state_t *cpm) {
  (void)cpu;
  uint8_t e = cpu->e;
  if (e == 0xFF) return cpm->current_user;
  cpm->current_user = e & 0x0F;
  return 0;
}

/* ── BDOS function dispatch ────────────────────────────────────────────── */

static int cpm_bdos_dispatch(z80_state_t *cpu, cpm_state_t *cpm) {
  uint8_t fn = cpu->c;
  uint16_t de = z80_de(cpu);
  int result = 0;
  int trap_rc = CPU_TRAP_HANDLED;

  cpm_trace_before(PPAP_TRACE_ABI_CPM_BDOS, fn, cpu);

  switch (fn) {
    case 0:
      trap_rc = CPU_TRAP_EXIT;
      break;
    case 1:
      result = cpm_console_input(cpu, cpm);
      break;
    case 2:
      cpm_console_output(cpu, cpm);
      break;
    case 3:
      result = cpm_reader_input(cpu, cpm);
      break;
    case 4:
      cpm_punch_output(cpu, cpm);
      break;
    case 5:
      cpm_list_output(cpu, cpm);
      break;
    case 6:
      result = cpm_direct_console_io(cpu, cpm);
      break;
    case 7:
      result = cpm_get_iobyte(cpu, cpm);
      break;
    case 8:
      cpm_set_iobyte(cpu, cpm);
      break;
    case 9:
      cpm_print_string(cpu, cpm);
      break;
    case 10:
      cpm_read_console_buffer(cpu, cpm);
      break;
    case 11:
      result = cpm_console_status(cpu, cpm);
      break;
    case 12:
      result = cpm_version_number(cpu, cpm);
      break;
    case 13:
      cpm_reset_disk(cpu, cpm);
      break;
    case 14:
      cpm_select_disk(cpu, cpm);
      break;
    case 15:
      result = cpm_open_file(cpu, cpm, de);
      break;
    case 16:
      result = cpm_close_file(cpu, cpm, de);
      break;
    case 17:
      result = cpm_search_first(cpu, cpm, de);
      break;
    case 18:
      result = cpm_search_next(cpu, cpm);
      break;
    case 19:
      result = cpm_delete_file(cpu, cpm, de);
      break;
    case 20:
      result = cpm_read_sequential(cpu, cpm, de);
      break;
    case 21:
      result = cpm_write_sequential(cpu, cpm, de);
      break;
    case 22:
      result = cpm_make_file(cpu, cpm, de);
      break;
    case 23:
      result = cpm_rename_file(cpu, cpm, de);
      break;
    case 24:
      result = cpm_return_login_vector(cpu, cpm);
      break;
    case 25:
      result = cpm_return_current_disk(cpu, cpm);
      break;
    case 26:
      cpm_set_dma_address(cpu, cpm);
      break;
    case 27:
      cpm_get_alloc_vector(cpu, cpm);
      trap_rc = CPU_TRAP_HANDLED;
      goto out;
    case 28:
      cpm_write_protect_disk(cpu, cpm);
      break;
    case 29:
      result = cpm_get_readonly_vector(cpu, cpm);
      break;
    case 30:
      result = cpm_set_file_attributes(cpu, cpm);
      break;
    case 31:
      cpm_get_dpb(cpu, cpm);
      trap_rc = CPU_TRAP_HANDLED;
      goto out;
    case 32:
      result = cpm_set_get_user_code(cpu, cpm);
      break;
    case 33:
      result = cpm_read_random(cpu, cpm, de);
      break;
    case 34:
      result = cpm_write_random(cpu, cpm, de);
      break;
    case 35:
      result = cpm_compute_file_size(cpu, cpm, de);
      break;
    case 36:
      result = cpm_set_random_record(cpu, cpm, de);
      break;
    case 40:
      result = cpm_write_random(cpu, cpm, de);
      break; /* zero fill = same */
    default:
      result = 0xFF;
      break;
  }

  cpu->a = result & 0xFF;
  cpu->l = result & 0xFF;
  cpu->h = (result >> 8) & 0xFF;

out:
  cpm_trace_after(PPAP_TRACE_ABI_CPM_BDOS, fn, cpu);
  return trap_rc;
}

/* ── BIOS dispatch ─────────────────────────────────────────────────────── */

static int cpm_bios_dispatch(z80_state_t *cpu, cpm_state_t *cpm, int bios_fn) {
  (void)cpm;
  int trap_rc;

  cpm_trace_before(PPAP_TRACE_ABI_CPM_BIOS, (uint32_t)bios_fn, cpu);

  switch (bios_fn) {
    case 0:
    case 1:
      trap_rc = CPU_TRAP_EXIT;
      break;
    case 2:
      cpu->a = cpm_char_ready() ? 0xFF : 0x00;
      trap_rc = CPU_TRAP_HANDLED;
      break;
    case 3:
      cpu->a = cpm_getchar();
      trap_rc = CPU_TRAP_HANDLED;
      break;
    case 4:
      cpm_putchar(cpu->c);
      trap_rc = CPU_TRAP_HANDLED;
      break;
    case 5:
      cpm_putchar(cpu->c);
      trap_rc = CPU_TRAP_HANDLED;
      break;
    case 6:
      trap_rc = CPU_TRAP_HANDLED;
      break;
    case 7:
      cpu->a = 0x1A;
      trap_rc = CPU_TRAP_HANDLED;
      break;
    case 15:
      cpu->a = 0xFF;
      trap_rc = CPU_TRAP_HANDLED;
      break;
    case 16:
      z80_set_hl(cpu, z80_bc(cpu));
      trap_rc = CPU_TRAP_HANDLED;
      break;
    default:
      trap_rc = CPU_TRAP_HANDLED;
      break;
  }

  cpm_trace_after(PPAP_TRACE_ABI_CPM_BIOS, (uint32_t)bios_fn, cpu);
  return trap_rc;
}

/* ── Trap handler ──────────────────────────────────────────────────────── */

int cpm_trap_handler(cpu_state_t *state, int trap_type, uint32_t param,
                     void *ctx) {
  z80_state_t *cpu = (z80_state_t *)state;
  cpm_state_t *cpm = (cpm_state_t *)ctx;

  if (trap_type == CPU_TRAP_RST) {
    /*
     * RST 0 from the stub area at CPM_STUB_BASE.
     * Slot 0 = BDOS, slots 1–17 = BIOS functions.
     * cpu->pc points past the RST byte, so:
     *   slot = (cpu->pc - 1 - CPM_STUB_BASE) / 2
     */
    if (param == 0x0000) {
      uint16_t rst_addr = cpu->pc - 1;
      if (rst_addr >= CPM_STUB_BASE &&
          rst_addr < CPM_STUB_BASE + (1 + CPM_BIOS_FN_COUNT) * 2) {
        int slot = (rst_addr - CPM_STUB_BASE) / 2;
        if (slot == 0) return cpm_bdos_dispatch(cpu, cpm);
        return cpm_bios_dispatch(cpu, cpm, slot - 1);
      }
      /* RST 0 not from stub area — exit */
      return CPU_TRAP_EXIT;
    }
  }

  if (trap_type == CPU_TRAP_HALT) return CPU_TRAP_EXIT;

  return CPU_TRAP_UNHANDLED;
}

/* ── Kernel-only: process entry point and subsystem ops ────────────────── */

#ifdef PPAP_KERNEL

#include "kernel/proc/proc.h"
#include "kernel/proc/sched.h"
#include "subsys.h"

/*
 * cpm_run_process — kernel-mode entry point for CP/M .COM processes.
 *
 * The scheduler "returns" into this function after proc_setup_stack().
 * It runs the Z80 emulator loop until BDOS fn 0 (or HALT) causes
 * ecpu_z80_ops.run() to return, then exits the process.
 */
void cpm_run_process(void) {
  pcb_t *p = current;

  /* Recover per-process state from subsys_data (set by exec_cpm) */
  /* cpm_exec_state_t layout: { z80_state_t z80; cpm_state_t cpm; } */
  z80_state_t *z80 = (z80_state_t *)p->subsys_data;
  cpm_state_t *cpm =
      (cpm_state_t *)((char *)p->subsys_data + sizeof(z80_state_t));

  /* Set per-process context so the terminal translator can mirror
   * the auto-detected dialect into cpm_state_t for /proc visibility. */
  adm_cpm_ctx = cpm;

  /* Switch TTY to raw mode: CP/M apps do their own echo and line
   * editing, so we need characters delivered one-at-a-time without
   * kernel echo or CR→LF translation.  Save original state for
   * restoration on exit (via cpm_on_exit). */
  {
    struct {
      uint32_t iflag, oflag, cflag, lflag;
      uint8_t line;
      uint8_t cc[19];
    } t;
    cpm_fd_ioctl(0, 0x5401 /*TCGETS*/, &t);
    cpm->saved_termios[0] = t.iflag;
    cpm->saved_termios[1] = t.oflag;
    cpm->saved_termios[2] = t.cflag;
    cpm->saved_termios[3] = t.lflag;
    cpm->saved_termios[4] = t.line;
    __builtin_memcpy(cpm->saved_termios_cc, t.cc, 19);
    cpm->termios_saved = 1;
    t.lflag &= ~0x000Au; /* clear ICANON (0x02) | ECHO (0x08) */
    t.iflag &= ~0x0100u; /* clear ICRNL */
    cpm_fd_ioctl(0, 0x5402 /*TCSETS*/, &t);
  }

  for (;;) {
    int do_step_stop = p->trace_step_pending != 0;
    int run_with_step =
        do_step_stop || (p->tracer_pid != 0 && trace_has_swbp());

    if (run_with_step) {
      p->trace_step_pending = 0;

      if (trace_maybe_stop_at_swbp(PPAP_TRACE_ABI_CPM_BDOS, z80->pc)) {
        if (p->state == PROC_TRACED_STOP) sched_switch();
        continue;
      }

      z80->step_budget = 1;
      ecpu_z80_ops.run((cpu_state_t *)z80);
      if (z80->step_trap_exit) break;
      if (do_step_stop && p->tracer_pid != 0 && p->state == PROC_RUNNABLE)
        trace_debug_stop(PPAP_TRACE_ABI_CPM_BDOS, z80->pc,
                         PPAP_DEBUG_STOP_STEP);
      if (p->state == PROC_TRACED_STOP) sched_switch();
      continue;
    }

    ecpu_z80_ops.run((cpu_state_t *)z80);
    break;
  }

  sys_exit(0);
  /* not reached */
}

/*
 * cpm_on_exit — restore terminal state before the process's fds are closed.
 *
 * Called from sys_exit() via the subsys on_exit hook.  This covers both
 * normal exit (BDOS fn 0) and signal-killed paths (SIGINT, SIGKILL, etc.).
 */
static void cpm_on_exit(struct pcb *p) {
  if (!p->subsys_data) return;

  cpm_state_t *cpm =
      (cpm_state_t *)((char *)p->subsys_data + sizeof(z80_state_t));
  if (!cpm->termios_saved) return;

  struct {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t line;
    uint8_t cc[19];
  } t;
  t.iflag = cpm->saved_termios[0];
  t.oflag = cpm->saved_termios[1];
  t.cflag = cpm->saved_termios[2];
  t.lflag = cpm->saved_termios[3];
  t.line = (uint8_t)cpm->saved_termios[4];
  __builtin_memcpy(t.cc, cpm->saved_termios_cc, 19);
  cpm_fd_ioctl(0, 0x5402 /*TCSETS*/, &t);
  cpm->termios_saved = 0;
}

static int cpm_proc_read(struct pcb *p, const char *name, char *buf,
                         int bufsiz) {
  if (name[0] != 't' || name[1] != 'e' || name[2] != 'r' || name[3] != 'm' ||
      name[4] != 'c' || name[5] != 'o' || name[6] != 'n' || name[7] != 'v' ||
      name[8] != '\0')
    return 0;

  if (!p->subsys_data) return 0;

  cpm_state_t *cpm =
      (cpm_state_t *)((char *)p->subsys_data + sizeof(z80_state_t));
  const char *s;
  switch (cpm->term_dialect) {
    case TERM_VT52:
      s = "vt52\n";
      break;
    case TERM_KAYPRO:
      s = "kaypro\n";
      break;
    default:
      s = "passthrough\n";
      break;
  }
  int len = 0;
  while (s[len] && len < bufsiz - 1) buf[len] = s[len], len++;
  buf[len] = '\0';
  return len;
}

/* Subsystem ops — CP/M processes don't need special crash/signal handling
 * since they run inside the Z80 emulator (faults are emulator bugs). */
const subsys_ops_t cpm_subsys_ops = {
    .on_crash = (void *)0,
    .on_signal = (void *)0,
    .on_init = (void *)0,
    .on_exit = cpm_on_exit,
    .on_proc_read = cpm_proc_read,
};

#endif /* PPAP_KERNEL */
