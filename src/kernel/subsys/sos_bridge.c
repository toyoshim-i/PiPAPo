/*
 * sos_bridge.c — S-OS "SWORD" API bridge (personality layer)
 *
 * Translates S-OS monitor calls from Z80-emulated programs into PPAP
 * syscalls.  Monitor entries (0x1F80–0x1FFD) and extended API
 * (0x2000–0x2036) contain JP instructions that redirect to an internal
 * RST stub area.  Only RST 0 and RST 18h fire traps — regular CALL
 * instructions pass through unhandled.
 */

#include "sos_bridge.h"

#include <string.h>

#include "common/ptrace.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/proc/proc.h"
#include "kernel/common/errno.h"
#include "kernel/mm/mem_region.h"

/* ── _SOS header parsing ───────────────────────────────────────────────── */

static int hex_digit(uint8_t ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  return -1;
}

static long sos_fd_desc(long fd) {
  if (fd < 0 || (uint32_t)fd >= FD_MAX) return -(long)EBADF;
  if (current->fd_map[(uint32_t)fd] == FD_DESC_NONE) return -(long)EBADF;
  return current->fd_map[(uint32_t)fd];
}

static int sos_page_ref(const void *buf, page_id_t *page, uint16_t *off) {
  return (mem_region_ptr_ref(buf, page, off) < 0) ? -EFAULT : 0;
}

static long sos_fd_read(long fd, void *buf, size_t n) {
  long desc = sos_fd_desc(fd);
  page_id_t page;
  uint16_t off;
  int rc;

  if (desc < 0) return desc;
  rc = sos_page_ref(buf, &page, &off);
  if (rc < 0) return rc;
  return mod_vfs.fd_read((int)desc, page, off, n);
}

static long sos_fd_write(long fd, const void *buf, size_t n) {
  long desc = sos_fd_desc(fd);
  page_id_t page;
  uint16_t off;
  int rc;

  if (desc < 0) return desc;
  rc = sos_page_ref(buf, &page, &off);
  if (rc < 0) return rc;
  return mod_vfs.fd_write((int)desc, page, off, n);
}

static int sos_fd_poll(long fd) {
  long desc = sos_fd_desc(fd);
  if (desc < 0) return (int)desc;
  return mod_vfs.fd_poll((int)desc);
}

static int sos_fd_ioctl(long fd, uint32_t cmd, void *arg) {
  long desc = sos_fd_desc(fd);
  if (desc < 0) return (int)desc;
  return mod_vfs.fd_ioctl((int)desc, cmd, arg);
}

static int hex2byte(const uint8_t *p) {
  int hi = hex_digit(p[0]);
  int lo = hex_digit(p[1]);
  if (hi < 0 || lo < 0) return -1;
  return (hi << 4) | lo;
}

static int hex4_to_u16(const uint8_t *p, uint16_t *out) {
  int b1 = hex2byte(p);
  int b0 = hex2byte(p + 2);
  if (b1 < 0 || b0 < 0) return -1;
  *out = (uint16_t)((b1 << 8) | b0);
  return 0;
}

int sos_parse_header(const uint8_t *file, uint32_t size, sos_header_t *hdr) {
  if (size < SOS_HEADER_SIZE) return -1;
  if (memcmp(file, SOS_MAGIC, SOS_MAGIC_LEN) != 0) return -1;
  if (file[4] != ' ' || file[7] != ' ' || file[12] != ' ') return -1;
  if (file[17] != 0x0A) return -1;

  int mode = hex2byte(file + 5);
  if (mode < 0) return -1;
  hdr->file_mode = (uint8_t)mode;

  if (hex4_to_u16(file + 8, &hdr->load_addr) < 0) return -1;
  if (hex4_to_u16(file + 13, &hdr->exec_addr) < 0) return -1;

  return 0;
}

/* ── Everything below is kernel-only (no host-test support yet) ────────── */

#ifdef PPAP_KERNEL

#include "common/poll.h"
#include "kernel/fd/fd.h"
#include "kernel/signal/signal.h"
#include "kernel/syscall/syscall.h"
#include "kernel/common/mod/mod_vfs.h"

static void sos_trace_before(uint32_t abi, uint32_t nr, z80_state_t *cpu) {
  (void)trace_before_subsys(abi, nr, z80_af(cpu), z80_bc(cpu), z80_de(cpu),
                            z80_hl(cpu), cpu->sp, cpu->pc);
}

static void sos_trace_after(uint32_t abi, uint32_t nr, z80_state_t *cpu) {
  trace_after_subsys(abi, nr, z80_af(cpu), z80_bc(cpu), z80_de(cpu),
                     z80_hl(cpu), cpu->sp, cpu->pc, (int32_t)cpu->a);
}

static void sos_raw_putchar(uint8_t ch) { sos_fd_write(1, &ch, 1); }

static void sos_putstr(const char *s, int len) {
  for (int i = 0; i < len; i++) sos_raw_putchar((uint8_t)s[i]);
}

/* ── Screen buffer helpers ───────────────────────────────────────────── */

static void sos_screen_scroll_up(sos_state_t *sos) {
  int w = sos->screen_width;
  int h = sos->screen_height;
  for (int y = 1; y < h; y++)
    __builtin_memcpy(&sos->screen_buf[(y - 1) * w], &sos->screen_buf[y * w],
                     (size_t)w);
  __builtin_memset(&sos->screen_buf[(h - 1) * w], ' ', (size_t)w);
}

static void sos_screen_putc(sos_state_t *sos, uint8_t ch) {
  int w = sos->screen_width;
  int h = sos->screen_height;

  if (ch == '\r') {
    sos->cursor_x = 0;
    return;
  }
  if (ch == '\n') {
    sos->cursor_y++;
    if (sos->cursor_y >= h) {
      sos_screen_scroll_up(sos);
      sos->cursor_y = h - 1;
    }
    return;
  }
  if (ch < 0x20) return; /* non-printable control chars don't go into buffer */

  if (sos->cursor_x < w && sos->cursor_y < h)
    sos->screen_buf[sos->cursor_y * w + sos->cursor_x] = ch;
  sos->cursor_x++;
  if (sos->cursor_x >= w) {
    sos->cursor_x = 0;
    sos->cursor_y++;
    if (sos->cursor_y >= h) {
      sos_screen_scroll_up(sos);
      sos->cursor_y = h - 1;
    }
  }
}

static uint8_t sos_getchar(void) {
  uint8_t ch = 0;
  for (;;) {
    long rc = sos_fd_read(0, &ch, 1);
    if (rc > 0) return ch;
    signal_check_kernel();
  }
}

static int sos_char_ready(void) {
  int mask = sos_fd_poll(0);
  if (mask < 0) return 0;
  return (mask & POLLIN) ? 1 : 0;
}

static int sos_file_open(const char *path, int flags) {
  return (int)sys_open((uintptr_t)path, (long)flags, 0644);
}

static int sos_file_close(int fd) { return (int)sys_close((long)fd); }

static int sos_file_read(int fd, void *buf, int count) {
  return (int)sos_fd_read((long)fd, buf, (size_t)count);
}

static int sos_file_write(int fd, const void *buf, int count) {
  return (int)sos_fd_write((long)fd, buf, (size_t)count);
}

static int sos_file_delete(const char *path) {
  return (int)sys_unlink((uintptr_t)path);
}

static int sos_file_rename(const char *oldpath, const char *newpath) {
  return (int)sys_rename((uintptr_t)oldpath, (uintptr_t)newpath);
}

/* ── Helper: read/write 16-bit LE from Z80 memory ────────────────────── */

static uint16_t z80_rd16(z80_state_t *cpu, uint16_t addr) {
  return (uint16_t)(cpu->memory[addr] | (cpu->memory[addr + 1] << 8));
}

static void z80_wr16(z80_state_t *cpu, uint16_t addr, uint16_t val) {
  cpu->memory[addr] = val & 0xFF;
  cpu->memory[addr + 1] = (val >> 8) & 0xFF;
}

/* ── Helper: resolve S-OS filename to PPAP path ──────────────────────── */

static int sos_drive_root_path(const sos_state_t *sos, uint8_t drive,
                               char *path, int path_size) {
  if (drive == 0 && sos->drive_a_root[0]) {
    int n = 0;
    while (sos->drive_a_root[n] && n < path_size - 1) {
      path[n] = sos->drive_a_root[n];
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

static void sos_resolve_path(z80_state_t *cpu, sos_state_t *sos, char *path,
                             int path_size) {
  char fnam[17];
  int i;
  for (i = 0; i < 16; i++) {
    char ch = (char)cpu->memory[SOS_FNAM + i];
    if (ch == '\0' || ch == ' ' || ch == '\r' || ch == '\n') break;
    fnam[i] = ch;
  }
  fnam[i] = '\0';

  uint8_t session = cpu->memory[SOS_DSK];
  if (session > 25) session = sos->current_session;

  int pos = sos_drive_root_path(sos, session, path, path_size);
  if (pos < path_size - 1) path[pos++] = '/';
  for (int j = 0; fnam[j] && pos < path_size - 1; j++) path[pos++] = fnam[j];
  path[pos] = '\0';
}

/* ── S-OS API function handlers ──────────────────────────────────────── */

/* fn 0: #COLD — cold start (exit) */
static int sos_fn_cold(z80_state_t *cpu, sos_state_t *sos) {
  (void)cpu;
  (void)sos;
  return CPU_TRAP_EXIT;
}

/* fn 1: #HOT — warm start (exit) */
static int sos_fn_hot(z80_state_t *cpu, sos_state_t *sos) {
  (void)cpu;
  (void)sos;
  return CPU_TRAP_EXIT;
}

/* fn 2: #VER — return machine ID in H, version in L */
static int sos_fn_ver(z80_state_t *cpu, sos_state_t *sos) {
  (void)sos;
  cpu->h = 0x66; /* machine ID */
  cpu->l = 0x20; /* version: SWORD 2.0 */
  return CPU_TRAP_HANDLED;
}

/* fn 3: #PRINT — print character in A */
static int sos_fn_print(z80_state_t *cpu, sos_state_t *sos) {
  uint8_t ch = cpu->a;
  sos_raw_putchar(ch);
  sos_screen_putc(sos, ch);
  return CPU_TRAP_HANDLED;
}

/* fn 4: #PRINTS — print a space character */
static int sos_fn_prints(z80_state_t *cpu, sos_state_t *sos) {
  (void)cpu;
  sos_raw_putchar(' ');
  sos_screen_putc(sos, ' ');
  return CPU_TRAP_HANDLED;
}

/* fn 5: #LTNL — line feed (LF) */
static int sos_fn_ltnl(z80_state_t *cpu, sos_state_t *sos) {
  (void)cpu;
  sos_raw_putchar('\n');
  sos_screen_putc(sos, '\n');
  return CPU_TRAP_HANDLED;
}

/* fn 6: #NL — newline (CR + LF) */
static int sos_fn_nl(z80_state_t *cpu, sos_state_t *sos) {
  (void)cpu;
  sos_putstr("\r\n", 2);
  sos_screen_putc(sos, '\r');
  sos_screen_putc(sos, '\n');
  return CPU_TRAP_HANDLED;
}

/* fn 7: #MSG — print string at DE, terminated by 0Dh (CR) */
static int sos_fn_msg(z80_state_t *cpu, sos_state_t *sos) {
  uint16_t addr = z80_de(cpu);
  while (addr < cpu->mem_size && cpu->memory[addr] != 0x0D) {
    uint8_t ch = cpu->memory[addr];
    sos_raw_putchar(ch);
    sos_screen_putc(sos, ch);
    addr++;
  }
  return CPU_TRAP_HANDLED;
}

/* fn 8: #MSX — print string at DE, terminated by 00h */
static int sos_fn_msx(z80_state_t *cpu, sos_state_t *sos) {
  uint16_t addr = z80_de(cpu);
  while (addr < cpu->mem_size && cpu->memory[addr] != '\0') {
    uint8_t ch = cpu->memory[addr];
    sos_raw_putchar(ch);
    sos_screen_putc(sos, ch);
    addr++;
  }
  return CPU_TRAP_HANDLED;
}

/* fn 9: #MPRINT — print inline string after CALL, terminated by 00h.
 * Pops the return address (which points to the inline string data),
 * prints until NUL, then pushes the address past the NUL as the new
 * return address so execution continues after the string. */
static int sos_fn_mprint(z80_state_t *cpu, sos_state_t *sos) {
  uint16_t addr = z80_pop16(cpu);
  while (addr < cpu->mem_size && cpu->memory[addr] != '\0') {
    uint8_t ch = cpu->memory[addr];
    sos_raw_putchar(ch);
    sos_screen_putc(sos, ch);
    addr++;
  }
  if (addr < cpu->mem_size) addr++; /* skip past NUL terminator */
  z80_push16(cpu, addr);
  return CPU_TRAP_HANDLED;
}

/* fn 10: #TAB — move cursor to column in B (ANSI escape) */
static int sos_fn_tab(z80_state_t *cpu, sos_state_t *sos) {
  sos->cursor_x = cpu->b;
  char buf[16];
  unsigned col = cpu->b + 1; /* ANSI is 1-based */
  int len = 0;
  buf[len++] = '\033';
  buf[len++] = '[';
  if (col >= 100) buf[len++] = '0' + col / 100;
  if (col >= 10) buf[len++] = '0' + (col / 10) % 10;
  buf[len++] = '0' + col % 10;
  buf[len++] = 'G';
  sos_putstr(buf, len);
  return CPU_TRAP_HANDLED;
}

/* fn 11: #LPRINT — line printer print (stub, redirect to stdout) */
static int sos_fn_lprint(z80_state_t *cpu, sos_state_t *sos) {
  sos_raw_putchar(cpu->a);
  sos_screen_putc(sos, cpu->a);
  return CPU_TRAP_HANDLED;
}

/* fn 12: #LPTON — line printer on (stub) */
static int sos_fn_lpton(z80_state_t *cpu, sos_state_t *sos) {
  (void)cpu;
  (void)sos;
  return CPU_TRAP_HANDLED;
}

/* fn 13: #LPTOF — line printer off (stub) */
static int sos_fn_lptof(z80_state_t *cpu, sos_state_t *sos) {
  (void)cpu;
  (void)sos;
  return CPU_TRAP_HANDLED;
}

/* fn 14: #GETL — line input: DE=buffer, B=max length */
static int sos_fn_getl(z80_state_t *cpu, sos_state_t *sos) {
  (void)sos;
  uint16_t buf_addr = z80_de(cpu);
  uint8_t max = cpu->b;
  if (max == 0) max = 255;

  int pos = 0;
  for (;;) {
    uint8_t ch = sos_getchar();
    if (ch == '\r' || ch == '\n') {
      sos_raw_putchar('\r');
      sos_raw_putchar('\n');
      break;
    }
    if ((ch == 0x08 || ch == 0x7F) && pos > 0) {
      pos--;
      sos_raw_putchar(0x08);
      sos_raw_putchar(' ');
      sos_raw_putchar(0x08);
      continue;
    }
    if (pos < max) {
      cpu->memory[buf_addr + pos] = ch;
      pos++;
      sos_raw_putchar(ch);
    }
  }
  cpu->memory[buf_addr + pos] = '\0';
  cpu->b = (uint8_t)pos;
  cpu->f &= ~FLAG_C;
  return CPU_TRAP_HANDLED;
}

/* fn 15: #GETKY — scan keyboard (non-blocking), A=key or 0 */
static int sos_fn_getky(z80_state_t *cpu, sos_state_t *sos) {
  (void)sos;
  if (sos_char_ready()) {
    cpu->a = sos_getchar();
  } else {
    cpu->a = 0;
  }
  cpu->f &= ~FLAG_C;
  return CPU_TRAP_HANDLED;
}

/* fn 16: #BRKEY — check break key (Z flag set if break) */
static int sos_fn_brkey(z80_state_t *cpu, sos_state_t *sos) {
  (void)sos;
  /* Z flag set = break detected */
  if (sos_char_ready()) {
    cpu->f |= FLAG_Z; /* Z set: break pending */
  } else {
    cpu->f &= ~FLAG_Z; /* Z clear: no break */
  }
  return CPU_TRAP_HANDLED;
}

/* fn 17: #INKEY — blocking key input, A=key */
static int sos_fn_inkey(z80_state_t *cpu, sos_state_t *sos) {
  (void)sos;
  cpu->a = sos_getchar();
  return CPU_TRAP_HANDLED;
}

/* fn 18: #PAUSE — wait for any key */
static int sos_fn_pause(z80_state_t *cpu, sos_state_t *sos) {
  (void)sos;
  (void)sos_getchar();
  cpu->f &= ~FLAG_C;
  return CPU_TRAP_HANDLED;
}

/* fn 19: #BELL — sound bell */
static int sos_fn_bell(z80_state_t *cpu, sos_state_t *sos) {
  (void)cpu;
  (void)sos;
  sos_raw_putchar('\a');
  return CPU_TRAP_HANDLED;
}

/* fn 20: #PRTHX — print A as 2-digit hex */
static int sos_fn_prthx(z80_state_t *cpu, sos_state_t *sos) {
  static const char hex[] = "0123456789ABCDEF";
  char buf[2];
  buf[0] = hex[(cpu->a >> 4) & 0x0F];
  buf[1] = hex[cpu->a & 0x0F];
  sos_putstr(buf, 2);
  sos_screen_putc(sos, (uint8_t)buf[0]);
  sos_screen_putc(sos, (uint8_t)buf[1]);
  return CPU_TRAP_HANDLED;
}

/* fn 21: #PRTHL — print HL as 4-digit hex */
static int sos_fn_prthl(z80_state_t *cpu, sos_state_t *sos) {
  static const char hex[] = "0123456789ABCDEF";
  uint16_t hl = z80_hl(cpu);
  char buf[4];
  buf[0] = hex[(hl >> 12) & 0x0F];
  buf[1] = hex[(hl >> 8) & 0x0F];
  buf[2] = hex[(hl >> 4) & 0x0F];
  buf[3] = hex[hl & 0x0F];
  sos_putstr(buf, 4);
  for (int i = 0; i < 4; i++) sos_screen_putc(sos, (uint8_t)buf[i]);
  return CPU_TRAP_HANDLED;
}

/* fn 22: #ASC — convert 4-bit value (low nibble of A) to ASCII '0'-'F' */
static int sos_fn_asc(z80_state_t *cpu, sos_state_t *sos) {
  (void)sos;
  static const char hex[] = "0123456789ABCDEF";
  cpu->a = (uint8_t)hex[cpu->a & 0x0F];
  return CPU_TRAP_HANDLED;
}

/* fn 23: #HEX — convert ASCII hex char in A to 4-bit value */
static int sos_fn_hex(z80_state_t *cpu, sos_state_t *sos) {
  (void)sos;
  int val = hex_digit(cpu->a);
  if (val >= 0) {
    cpu->a = (uint8_t)val;
    cpu->f &= ~FLAG_C;
  } else {
    cpu->f |= FLAG_C;
  }
  return CPU_TRAP_HANDLED;
}

/* fn 24: #2HEX — 2 hex chars at (HL) to byte in A */
static int sos_fn_2hex(z80_state_t *cpu, sos_state_t *sos) {
  (void)sos;
  uint16_t addr = z80_hl(cpu);
  int val = hex2byte(&cpu->memory[addr]);
  if (val >= 0) {
    cpu->a = (uint8_t)val;
    cpu->f &= ~FLAG_C;
  } else {
    cpu->f |= FLAG_C;
  }
  return CPU_TRAP_HANDLED;
}

/* fn 25: #HLHEX — 4 hex chars at (DE) to 16-bit value in HL */
static int sos_fn_hlhex(z80_state_t *cpu, sos_state_t *sos) {
  (void)sos;
  uint16_t de = z80_de(cpu);
  int b1 = hex2byte(&cpu->memory[de]);
  int b0 = hex2byte(&cpu->memory[de + 2]);
  if (b1 >= 0 && b0 >= 0) {
    uint16_t val = (uint16_t)((b1 << 8) | b0);
    cpu->h = (val >> 8) & 0xFF;
    cpu->l = val & 0xFF;
    cpu->f &= ~FLAG_C;
  } else {
    cpu->f |= FLAG_C;
  }
  return CPU_TRAP_HANDLED;
}

/* fn 26-29: #WOPEN, #WRD, #FCB, #RDD — file I/O (stubs) */
static int sos_fn_wopen(z80_state_t *cpu, sos_state_t *sos) {
  char path[64];
  sos_resolve_path(cpu, sos, path, sizeof(path));
  int fd = sos_file_open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    cpu->f |= FLAG_C;
  } else {
    sos->file_fd = fd;
    cpu->f &= ~FLAG_C;
  }
  return CPU_TRAP_HANDLED;
}

static int sos_fn_wrd(z80_state_t *cpu, sos_state_t *sos) {
  if (sos->file_fd < 0) {
    cpu->f |= FLAG_C;
    return CPU_TRAP_HANDLED;
  }
  uint16_t dtadr = z80_rd16(cpu, SOS_DTADR);
  uint16_t fsize = z80_rd16(cpu, SOS_SIZE);
  if (fsize == 0) {
    cpu->f |= FLAG_C;
    return CPU_TRAP_HANDLED;
  }
  uint32_t len = fsize;
  int written = sos_file_write(sos->file_fd, &cpu->memory[dtadr], (int)len);
  sos_file_close(sos->file_fd);
  sos->file_fd = -1;
  if (written < 0 || (uint32_t)written != len)
    cpu->f |= FLAG_C;
  else
    cpu->f &= ~FLAG_C;
  return CPU_TRAP_HANDLED;
}

static int sos_fn_rdd(z80_state_t *cpu, sos_state_t *sos) {
  if (sos->file_fd < 0) {
    cpu->f |= FLAG_C;
    return CPU_TRAP_HANDLED;
  }
  uint16_t dtadr = z80_rd16(cpu, SOS_DTADR);
  uint32_t max_len = cpu->mem_size - dtadr;
  int nread = sos_file_read(sos->file_fd, &cpu->memory[dtadr], (int)max_len);
  sos_file_close(sos->file_fd);
  sos->file_fd = -1;
  if (nread < 0) {
    cpu->f |= FLAG_C;
  } else {
    z80_wr16(cpu, SOS_SIZE, (uint16_t)nread);
    cpu->f &= ~FLAG_C;
  }
  return CPU_TRAP_HANDLED;
}

static int sos_fn_fcb(z80_state_t *cpu, sos_state_t *sos) {
  (void)cpu;
  (void)sos;
  cpu->f |= FLAG_C;
  return CPU_TRAP_HANDLED;
}

/* fn 30: #FILE — directory listing (stub) */
static int sos_fn_file(z80_state_t *cpu, sos_state_t *sos) {
  (void)cpu;
  (void)sos;
  cpu->f |= FLAG_C;
  return CPU_TRAP_HANDLED;
}

/* fn 31: #FSAME — filename compare (stub) */
static int sos_fn_fsame(z80_state_t *cpu, sos_state_t *sos) {
  (void)cpu;
  (void)sos;
  cpu->f |= FLAG_C;
  return CPU_TRAP_HANDLED;
}

/* fn 32: #FPRNT — file info print (stub) */
static int sos_fn_fprnt(z80_state_t *cpu, sos_state_t *sos) {
  (void)cpu;
  (void)sos;
  cpu->f |= FLAG_C;
  return CPU_TRAP_HANDLED;
}

/* fn 33: #POKE — store A at memory[HL] */
static int sos_fn_poke(z80_state_t *cpu, sos_state_t *sos) {
  (void)sos;
  uint16_t addr = z80_hl(cpu);
  cpu->memory[addr] = cpu->a;
  cpu->f &= ~FLAG_C;
  return CPU_TRAP_HANDLED;
}

/* fn 34: #POKE@ — block store: HL=dest addr, DE=src buf, BC=count */
static int sos_fn_pokea(z80_state_t *cpu, sos_state_t *sos) {
  (void)sos;
  uint16_t dest = z80_hl(cpu);
  uint16_t src = z80_de(cpu);
  uint16_t count = z80_bc(cpu);
  for (uint16_t i = 0; i < count && (uint32_t)(dest + i) < cpu->mem_size; i++)
    cpu->memory[dest + i] = cpu->memory[src + i];
  cpu->f &= ~FLAG_C;
  return CPU_TRAP_HANDLED;
}

/* fn 35: #PEEK — read memory[HL] into A */
static int sos_fn_peek(z80_state_t *cpu, sos_state_t *sos) {
  (void)sos;
  uint16_t addr = z80_hl(cpu);
  cpu->a = cpu->memory[addr];
  cpu->f &= ~FLAG_C;
  return CPU_TRAP_HANDLED;
}

/* fn 36: #PEEK@ — block read: HL=src addr, DE=dest buf, BC=count */
static int sos_fn_peeka(z80_state_t *cpu, sos_state_t *sos) {
  (void)sos;
  uint16_t src = z80_hl(cpu);
  uint16_t dest = z80_de(cpu);
  uint16_t count = z80_bc(cpu);
  for (uint16_t i = 0; i < count && (uint32_t)(src + i) < cpu->mem_size; i++)
    cpu->memory[dest + i] = cpu->memory[src + i];
  cpu->f &= ~FLAG_C;
  return CPU_TRAP_HANDLED;
}

/* fn 37: #MON — enter monitor (exit) */
static int sos_fn_mon(z80_state_t *cpu, sos_state_t *sos) {
  (void)cpu;
  (void)sos;
  return CPU_TRAP_EXIT;
}

/* Extended API stubs */

/* fn 42: #DIR — directory listing */
static int sos_fn_dir(z80_state_t *cpu, sos_state_t *sos) {
  (void)cpu;
  (void)sos;
  cpu->f |= FLAG_C;
  return CPU_TRAP_HANDLED;
}

/* fn 43: #ROPEN — file read open */
static int sos_fn_ropen(z80_state_t *cpu, sos_state_t *sos) {
  char path[64];
  sos_resolve_path(cpu, sos, path, sizeof(path));
  int fd = sos_file_open(path, O_RDONLY);
  if (fd < 0) {
    cpu->f |= FLAG_C;
  } else {
    sos->file_fd = fd;
    cpu->f &= ~FLAG_C;
  }
  return CPU_TRAP_HANDLED;
}

/* fn 46: #NAME — rename file */
static int sos_fn_name(z80_state_t *cpu, sos_state_t *sos) {
  char oldpath[64];
  sos_resolve_path(cpu, sos, oldpath, sizeof(oldpath));

  uint16_t de = z80_de(cpu);
  char newname[17];
  int i;
  for (i = 0; i < 16 && (uint32_t)(de + i) < cpu->mem_size; i++) {
    char ch = (char)cpu->memory[de + i];
    if (ch == '\0' || ch == ' ' || ch == '\r' || ch == '\n') break;
    newname[i] = ch;
  }
  newname[i] = '\0';

  char newpath[64];
  uint8_t session = cpu->memory[SOS_DSK];
  if (session > 25) session = sos->current_session;
  char drive = 'a' + session;
  int pos = 0;
  newpath[pos++] = '/';
  newpath[pos++] = drive;
  newpath[pos++] = '/';
  for (int j = 0; newname[j] && pos < 63; j++) newpath[pos++] = newname[j];
  newpath[pos] = '\0';

  if (sos_file_rename(oldpath, newpath) < 0)
    cpu->f |= FLAG_C;
  else
    cpu->f &= ~FLAG_C;
  return CPU_TRAP_HANDLED;
}

/* fn 47: #KILL — delete file */
static int sos_fn_kill(z80_state_t *cpu, sos_state_t *sos) {
  char path[64];
  sos_resolve_path(cpu, sos, path, sizeof(path));

  if (sos_file_delete(path) < 0)
    cpu->f |= FLAG_C;
  else
    cpu->f &= ~FLAG_C;
  return CPU_TRAP_HANDLED;
}

/* fn 48: #CSR — get cursor position, returns L=X, H=Y */
static int sos_fn_csr(z80_state_t *cpu, sos_state_t *sos) {
  cpu->l = sos->cursor_x;
  cpu->h = sos->cursor_y;
  return CPU_TRAP_HANDLED;
}

/* fn 49: #SCRN — read character at position (H=Y, L=X), returns A */
static int sos_fn_scrn(z80_state_t *cpu, sos_state_t *sos) {
  int x = cpu->l;
  int y = cpu->h;
  if (x < sos->screen_width && y < sos->screen_height)
    cpu->a = sos->screen_buf[y * sos->screen_width + x];
  else
    cpu->a = ' ';
  return CPU_TRAP_HANDLED;
}

/* fn 50: #LOC — set cursor to (L=X, H=Y) via ANSI escape */
static int sos_fn_loc(z80_state_t *cpu, sos_state_t *sos) {
  if (cpu->l >= sos->screen_width || cpu->h >= sos->screen_height) {
    cpu->f |= FLAG_C;
    return CPU_TRAP_HANDLED;
  }
  sos->cursor_x = cpu->l;
  sos->cursor_y = cpu->h;
  uint8_t row = cpu->h + 1; /* ANSI is 1-based */
  uint8_t col = cpu->l + 1;
  char buf[16];
  int len = 0;
  buf[len++] = '\033';
  buf[len++] = '[';
  if (row >= 100) buf[len++] = '0' + row / 100;
  if (row >= 10) buf[len++] = '0' + (row / 10) % 10;
  buf[len++] = '0' + row % 10;
  buf[len++] = ';';
  if (col >= 100) buf[len++] = '0' + col / 100;
  if (col >= 10) buf[len++] = '0' + (col / 10) % 10;
  buf[len++] = '0' + col % 10;
  buf[len++] = 'H';
  sos_putstr(buf, len);
  cpu->f &= ~FLAG_C;
  return CPU_TRAP_HANDLED;
}

/* fn 56: #WIDCH — ignored; keep the user's current screen mode */
static int sos_fn_widch(z80_state_t *cpu, sos_state_t *sos) {
  (void)cpu;
  (void)sos;
  return CPU_TRAP_HANDLED;
}

/* Generic no-op stub for unimplemented functions */
static int sos_fn_stub(z80_state_t *cpu, sos_state_t *sos) {
  (void)sos;
  cpu->f |= FLAG_C;
  return CPU_TRAP_HANDLED;
}

/* ── S-OS API dispatch ─────────────────────────────────────────────────── */

static int sos_dispatch(uint32_t fn, z80_state_t *cpu, sos_state_t *sos) {
  sos_trace_before(PPAP_TRACE_ABI_SOS, fn, cpu);

  int rc;
  switch (fn) {
    case SOS_FN_COLD:
      rc = sos_fn_cold(cpu, sos);
      break;
    case SOS_FN_HOT:
      rc = sos_fn_hot(cpu, sos);
      break;
    case SOS_FN_VER:
      rc = sos_fn_ver(cpu, sos);
      break;
    case SOS_FN_PRINT:
      rc = sos_fn_print(cpu, sos);
      break;
    case SOS_FN_PRINTS:
      rc = sos_fn_prints(cpu, sos);
      break;
    case SOS_FN_LTNL:
      rc = sos_fn_ltnl(cpu, sos);
      break;
    case SOS_FN_NL:
      rc = sos_fn_nl(cpu, sos);
      break;
    case SOS_FN_MSG:
      rc = sos_fn_msg(cpu, sos);
      break;
    case SOS_FN_MSX:
      rc = sos_fn_msx(cpu, sos);
      break;
    case SOS_FN_MPRINT:
      rc = sos_fn_mprint(cpu, sos);
      break;
    case SOS_FN_TAB:
      rc = sos_fn_tab(cpu, sos);
      break;
    case SOS_FN_LPRINT:
      rc = sos_fn_lprint(cpu, sos);
      break;
    case SOS_FN_LPTON:
      rc = sos_fn_lpton(cpu, sos);
      break;
    case SOS_FN_LPTOF:
      rc = sos_fn_lptof(cpu, sos);
      break;
    case SOS_FN_GETL:
      rc = sos_fn_getl(cpu, sos);
      break;
    case SOS_FN_GETKY:
      rc = sos_fn_getky(cpu, sos);
      break;
    case SOS_FN_BRKEY:
      rc = sos_fn_brkey(cpu, sos);
      break;
    case SOS_FN_INKEY:
      rc = sos_fn_inkey(cpu, sos);
      break;
    case SOS_FN_PAUSE:
      rc = sos_fn_pause(cpu, sos);
      break;
    case SOS_FN_BELL:
      rc = sos_fn_bell(cpu, sos);
      break;
    case SOS_FN_PRTHX:
      rc = sos_fn_prthx(cpu, sos);
      break;
    case SOS_FN_PRTHL:
      rc = sos_fn_prthl(cpu, sos);
      break;
    case SOS_FN_ASC:
      rc = sos_fn_asc(cpu, sos);
      break;
    case SOS_FN_HEX:
      rc = sos_fn_hex(cpu, sos);
      break;
    case SOS_FN_2HEX:
      rc = sos_fn_2hex(cpu, sos);
      break;
    case SOS_FN_HLHEX:
      rc = sos_fn_hlhex(cpu, sos);
      break;
    case SOS_FN_WOPEN:
      rc = sos_fn_wopen(cpu, sos);
      break;
    case SOS_FN_WRD:
      rc = sos_fn_wrd(cpu, sos);
      break;
    case SOS_FN_FCB:
      rc = sos_fn_fcb(cpu, sos);
      break;
    case SOS_FN_RDD:
      rc = sos_fn_rdd(cpu, sos);
      break;
    case SOS_FN_FILE:
      rc = sos_fn_file(cpu, sos);
      break;
    case SOS_FN_FSAME:
      rc = sos_fn_fsame(cpu, sos);
      break;
    case SOS_FN_FPRNT:
      rc = sos_fn_fprnt(cpu, sos);
      break;
    case SOS_FN_POKE:
      rc = sos_fn_poke(cpu, sos);
      break;
    case SOS_FN_POKEA:
      rc = sos_fn_pokea(cpu, sos);
      break;
    case SOS_FN_PEEK:
      rc = sos_fn_peek(cpu, sos);
      break;
    case SOS_FN_PEEKA:
      rc = sos_fn_peeka(cpu, sos);
      break;
    case SOS_FN_MON:
      rc = sos_fn_mon(cpu, sos);
      break;
    /* Extended API */
    case SOS_FN_DIR:
      rc = sos_fn_dir(cpu, sos);
      break;
    case SOS_FN_ROPEN:
      rc = sos_fn_ropen(cpu, sos);
      break;
    case SOS_FN_NAME:
      rc = sos_fn_name(cpu, sos);
      break;
    case SOS_FN_KILL:
      rc = sos_fn_kill(cpu, sos);
      break;
    case SOS_FN_CSR:
      rc = sos_fn_csr(cpu, sos);
      break;
    case SOS_FN_SCRN:
      rc = sos_fn_scrn(cpu, sos);
      break;
    case SOS_FN_LOC:
      rc = sos_fn_loc(cpu, sos);
      break;
    case SOS_FN_FLGET:
      rc = sos_fn_inkey(cpu, sos);
      break;
    case SOS_FN_WIDCH:
      rc = sos_fn_widch(cpu, sos);
      break;
    case SOS_FN_BOOT:
      rc = sos_fn_cold(cpu, sos);
      break;
    default:
      if (fn < 32)
        sos->unsupported_lo |= (1u << fn);
      else if (fn < 59)
        sos->unsupported_hi |= (1u << (fn - 32));
      rc = sos_fn_stub(cpu, sos);
      break;
  }

  sos_trace_after(PPAP_TRACE_ABI_SOS, fn, cpu);
  return rc;
}

/* ── Trap handler ──────────────────────────────────────────────────────── */

int sos_trap_handler(cpu_state_t *state, int trap_type, uint32_t param,
                     void *ctx) {
  z80_state_t *cpu = (z80_state_t *)state;
  sos_state_t *sos = (sos_state_t *)ctx;

  if (trap_type == CPU_TRAP_RST) {
    /* RST 18h: A register has the function index */
    if (param == SOS_RST18_ADDR) return sos_dispatch(cpu->a, cpu, sos);

    /*
     * RST 0 from the internal stub area at SOS_STUB_BASE.
     * Monitor entries (0x1F80–0x1FFD) and extended API (0x2000–0x2036)
     * contain JP instructions that redirect to stubs at
     * SOS_STUB_BASE + fn * 2.  Each stub is { RST 0; RET }.
     * When RST 0 fires, cpu->pc points past the RST byte, so:
     *   fn = (cpu->pc - 1 - SOS_STUB_BASE) / 2
     */
    if (param == 0x0000) {
      uint16_t rst_addr = cpu->pc - 1;
      if (rst_addr >= SOS_STUB_BASE &&
          rst_addr < SOS_STUB_BASE + SOS_FN_MAX * 2) {
        uint32_t fn = (rst_addr - SOS_STUB_BASE) / 2;
        return sos_dispatch(fn, cpu, sos);
      }
      /* RST 0 not from stub area — cold start (exit) */
      return CPU_TRAP_EXIT;
    }
  }

  if (trap_type == CPU_TRAP_HALT) return CPU_TRAP_EXIT;

  return CPU_TRAP_UNHANDLED;
}

/* ── Kernel-mode process entry and subsys ops ─────────────────────────── */

#include "kernel/proc/proc.h"
#include "kernel/proc/sched.h"
#include "subsys.h"

void sos_run_process(void) {
  pcb_t *p = current;

  /* Recover per-process state: { z80_state_t, sos_state_t } */
  z80_state_t *z80 = (z80_state_t *)p->subsys_data;
  sos_state_t *sos =
      (sos_state_t *)((char *)p->subsys_data + sizeof(z80_state_t));

  /* Switch TTY to raw mode (same pattern as CP/M) */
  {
    struct {
      uint32_t iflag, oflag, cflag, lflag;
      uint8_t line;
      uint8_t cc[19];
    } t;
    sos_fd_ioctl(0, 0x5401 /*TCGETS*/, &t);
    sos->saved_termios[0] = t.iflag;
    sos->saved_termios[1] = t.oflag;
    sos->saved_termios[2] = t.cflag;
    sos->saved_termios[3] = t.lflag;
    sos->saved_termios[4] = t.line;
    __builtin_memcpy(sos->saved_termios_cc, t.cc, 19);
    sos->termios_saved = 1;
    t.lflag &= ~0x000Au; /* clear ICANON | ECHO */
    t.iflag &= ~0x0100u; /* clear ICRNL */
    sos_fd_ioctl(0, 0x5402 /*TCSETS*/, &t);
  }

  /* Clear screen before running the S-OS program */
  {
    static const char cls[] = "\033[2J\033[H";
    sos_putstr(cls, (int)(sizeof(cls) - 1));
  }

  for (;;) {
    int do_step_stop = p->trace_step_pending != 0;
    int run_with_step =
        do_step_stop || (p->tracer_pid != 0 && trace_has_swbp());

    if (run_with_step) {
      p->trace_step_pending = 0;

      if (trace_maybe_stop_at_swbp(PPAP_TRACE_ABI_SOS, z80->pc)) {
        if (p->state == PROC_TRACED_STOP) sched_yield();
        continue;
      }

      z80->step_budget = 1;
      ecpu_z80_ops.run((cpu_state_t *)z80);
      if (z80->step_trap_exit) break;
      if (do_step_stop && p->tracer_pid != 0 && p->state == PROC_RUNNABLE)
        trace_debug_stop(PPAP_TRACE_ABI_SOS, z80->pc, PPAP_DEBUG_STOP_STEP);
      if (p->state == PROC_TRACED_STOP) sched_yield();
      continue;
    }

    ecpu_z80_ops.run((cpu_state_t *)z80);
    break;
  }

  sys_exit(0);
}

static const char *sos_fn_name_table[] = {
    "#COLD",  "#HOT",   "#VER",    "#PRINT", "#PRINTS", "#LTNL",  "#NL",
    "#MSG",   "#MSX",   "#MPRINT", "#TAB",   "#LPRINT", "#LPTON", "#LPTOF",
    "#GETL",  "#GETKY", "#BRKEY",  "#INKEY", "#PAUSE",  "#BELL",  "#PRTHX",
    "#PRTHL", "#ASC",   "#HEX",    "#2HEX",  "#HLHEX",  "#WOPEN", "#WRD",
    "#FCB",   "#RDD",   "#FILE",   "#FSAME", "#FPRNT",  "#POKE",  "#POKEA",
    "#PEEK",  "#PEEKA", "#MON",    0,        0, /* 38, 39 — gap */
    "#DRDSB", "#DWTSB", "#DIR",    "#ROPEN", "#SET",    "#RESET", "#NAME",
    "#KILL",  "#CSR",   "#SCRN",   "#LOC",   "#FLGET",  "#RDVSW", "#SDVSW",
    "#INP",   "#OUT",   "#WIDCH",  "#ERROR", "#BOOT",
};

static void sos_print_fn(int i) {
  const char *name =
      (i < (int)(sizeof(sos_fn_name_table) / sizeof(sos_fn_name_table[0])))
          ? sos_fn_name_table[i]
          : 0;
  if (name) {
    int len = 0;
    while (name[len]) len++;
    for (int i = 0; i < len; i++) {
      char ch = name[i];
      sos_fd_write(2, &ch, 1);
    }
  } else {
    char buf[4];
    int pos = 0;
    int v = i;
    buf[pos++] = '#';
    int tens = 0;
    while (v >= 10) {
      tens++;
      v -= 10;
    }
    if (tens) buf[pos++] = '0' + (char)tens;
    buf[pos++] = '0' + (char)v;
    sos_fd_write(2, buf, (size_t)pos);
  }
}

static void sos_print_unsupported(sos_state_t *sos) {
  uint32_t lo = sos->unsupported_lo;
  uint32_t hi = sos->unsupported_hi;
  if (!lo && !hi) return;

  {
    char msg[] = "\nsos: unsupported API calls:";
    sos_fd_write(2, msg, 27);
  }
  for (int i = 0; i < 32 && lo; i++) {
    if (lo & (1u << i)) {
      {
        char sp = ' ';
        sos_fd_write(2, &sp, 1);
      }
      sos_print_fn(i);
      lo &= ~(1u << i);
    }
  }
  for (int i = 0; i < 27 && hi; i++) {
    if (hi & (1u << i)) {
      {
        char sp = ' ';
        sos_fd_write(2, &sp, 1);
      }
      sos_print_fn(i + 32);
      hi &= ~(1u << i);
    }
  }
  {
    char nl = '\n';
    sos_fd_write(2, &nl, 1);
  }
}

static void sos_on_exit(struct pcb *p) {
  if (!p->subsys_data) return;

  sos_state_t *sos =
      (sos_state_t *)((char *)p->subsys_data + sizeof(z80_state_t));

  sos_print_unsupported(sos);

  if (!sos->termios_saved) return;

  struct {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t line;
    uint8_t cc[19];
  } t;
  t.iflag = sos->saved_termios[0];
  t.oflag = sos->saved_termios[1];
  t.cflag = sos->saved_termios[2];
  t.lflag = sos->saved_termios[3];
  t.line = (uint8_t)sos->saved_termios[4];
  __builtin_memcpy(t.cc, sos->saved_termios_cc, 19);
  sos_fd_ioctl(0, 0x5402 /*TCSETS*/, &t);
  sos->termios_saved = 0;
}

const subsys_ops_t sos_subsys_ops = {
    .on_crash = (void *)0,
    .on_signal = (void *)0,
    .on_init = (void *)0,
    .on_exit = sos_on_exit,
};

#endif /* PPAP_KERNEL */
