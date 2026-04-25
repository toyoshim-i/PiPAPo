/*
 * dos_bridge.c --- MS-DOS personality subsystem bridge
 */

#include "kernel/core/subsys/msdos/dos_bridge.h"

#include <stddef.h>

#include "common/dirent.h"
#include "common/errno.h"
#include "common/fcntl.h"
#include "common/stat.h"
#include "common/termios.h"
#include "kernel/common/core/proc_info.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/cpu/cpu.h"
#include "kernel/core/exec/exec_args.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/proc/sched.h"
#include "kernel/core/subsys/msdos/dos_host.h"
#include "kernel/core/syscall/syscall.h"

/* Kernel-side scratch slots inside `dos_data_page` (allocated lazily in
 * msdos_on_init).  Layout:
 *   0x0000..DOS_IO_SCRATCH_OFF     dos_proc_t[PROC_MAX]    (hot, stack-copied)
 *   DOS_IO_SCRATCH_OFF            1-byte staging for fd 0/1 byte I/O
 *   DOS_PATH_SCRATCH_OFF          128-byte staging for resolved paths
 *   DOS_PATH_SCRATCH2_OFF         second 128-byte slot for RENAME
 *   DOS_DIRENT_SCRATCH_OFF        one struct dirent for AH=4Eh/4Fh scans
 *   DOS_STAT_FULL_OFF             128-byte full-path buffer for dos_stat_entry
 *   DOS_COLD_OFF..DOS_SCRATCH_END dos_proc_cold_t[PROC_MAX] (cold, page-only)
 *
 * DOS_IO_SCRATCH_OFF is sized to fit the hot dos_proc_t array with a
 * little headroom; shrinking the struct (by moving paths / termios to
 * cold) lets the cold array fit in a single PAGE_SIZE page. */
#define DOS_IO_SCRATCH_OFF 0x280u /* ≥ sizeof(dos_proc_t) * PROC_MAX */
#define DOS_PATH_SCRATCH_OFF (DOS_IO_SCRATCH_OFF + 16u)
#define DOS_PATH_SCRATCH_MAX 128u
#define DOS_PATH_SCRATCH2_OFF (DOS_PATH_SCRATCH_OFF + DOS_PATH_SCRATCH_MAX)
#define DOS_DIRENT_SCRATCH_OFF (DOS_PATH_SCRATCH2_OFF + DOS_PATH_SCRATCH_MAX)
#define DOS_STAT_FULL_OFF (DOS_DIRENT_SCRATCH_OFF + sizeof(struct dirent))
#define DOS_STAT_FULL_MAX DOS_PATH_SCRATCH_MAX
#define DOS_COLD_OFF (DOS_STAT_FULL_OFF + DOS_STAT_FULL_MAX)
#define DOS_PATHS_OFF (DOS_COLD_OFF + sizeof(dos_proc_cold_t) * PROC_MAX)
#define DOS_SCRATCH_END (DOS_PATHS_OFF + sizeof(dos_proc_paths_t) * PROC_MAX)

/* Field offsets inside the per-proc paths slot (used by callers that
 * access one path field via byte-level page I/O). */
#define DOS_EXEC_DIR_FIELD_OFF offsetof(dos_proc_paths_t, exec_dir)
#define DOS_CWD_C_FIELD_OFF offsetof(dos_proc_paths_t, cwd_c)
#define DOS_CWD_Z_FIELD_OFF offsetof(dos_proc_paths_t, cwd_z)

#define DOS_FIRST_USER_HANDLE 5

/* Dedicated page for dos_proc_t array to avoid BSS overflow in core segment.
 * Same page also hosts the DOS scratch area (paths + 1-byte I/O staging). */
static page_id_t dos_data_page = PAGE_ID_INVALID;

_Static_assert(sizeof(dos_proc_t) * PROC_MAX <= DOS_IO_SCRATCH_OFF,
               "dos_proc_t array would overlap DOS scratch area");
_Static_assert(DOS_SCRATCH_END <= PAGE_SIZE,
               "DOS scratch area exceeds dos_data_page");

static uint32_t dos_to_linear(uint16_t seg, uint16_t off) {
  return ((uint32_t)seg << 4) + off;
}

static void dos_get_proc(struct pcb *p, dos_proc_t *out) {
  uint32_t slot = (uint32_t)(p - proc_table);
  mem_region_page_read(dos_data_page, (uint16_t)(slot * sizeof(dos_proc_t)),
                       out, sizeof(dos_proc_t));
}

static void dos_put_proc(struct pcb *p, const dos_proc_t *in) {
  uint32_t slot = (uint32_t)(p - proc_table);
  mem_region_page_write(dos_data_page, (uint16_t)(slot * sizeof(dos_proc_t)),
                        in, sizeof(dos_proc_t));
}

/* Read / write the per-process dos_proc_cold_t slot directly on
 * dos_data_page.  Cold state (IVT saves, termios snapshot) is not
 * copied to the kernel stack — msdos_on_init / msdos_on_exit and
 * AH=25h are the only callers, all outside the deep VFS-lookup
 * chain. */
static void dos_get_cold(struct pcb *p, dos_proc_cold_t *out) {
  uint32_t slot = (uint32_t)(p - proc_table);
  mem_region_page_read(
      dos_data_page, (uint16_t)(DOS_COLD_OFF + slot * sizeof(dos_proc_cold_t)),
      out, sizeof(dos_proc_cold_t));
}

static void dos_put_cold(struct pcb *p, const dos_proc_cold_t *in) {
  uint32_t slot = (uint32_t)(p - proc_table);
  mem_region_page_write(
      dos_data_page, (uint16_t)(DOS_COLD_OFF + slot * sizeof(dos_proc_cold_t)),
      in, sizeof(dos_proc_cold_t));
}

/* Single-byte accessors for dos_data_page — used by path-building and
 * directory-scan helpers that work a byte at a time.  Keeping the
 * scratch in the page instead of on the kernel stack saves the
 * 128-byte local buffers those helpers used to carry. */
static void dos_scratch_putb(uint16_t off, uint8_t b) {
  mem_region_page_write(dos_data_page, off, &b, 1);
}

static uint8_t dos_scratch_getb(uint16_t off) {
  uint8_t b;
  mem_region_page_read(dos_data_page, off, &b, 1);
  return b;
}

/* Byte offset of process `p`'s cold slot in dos_data_page. */
static uint16_t dos_cold_base(struct pcb *p) {
  uint32_t slot = (uint32_t)(p - proc_table);
  return (uint16_t)(DOS_COLD_OFF + slot * sizeof(dos_proc_cold_t));
}

/* Byte offset of process `p`'s paths slot in dos_data_page. */
static uint16_t dos_paths_base(struct pcb *p) {
  uint32_t slot = (uint32_t)(p - proc_table);
  return (uint16_t)(DOS_PATHS_OFF + slot * sizeof(dos_proc_paths_t));
}

static int msdos_on_crash(struct pcb *p, uint32_t *regs, uint16_t *exc,
                          int is_group0) {
  return 0;
}

static int msdos_on_signal(struct pcb *p, int sig, uint32_t *regs) { return 0; }

/* Switch the process's stdin TTY into a DOS-friendly raw mode: Enter
 * delivered as CR (not LF), no kernel echo, no canonical line editing.
 * The pre-change flags are stashed in the cold per-proc slot so
 * msdos_on_exit can restore them — matches the CP/M subsystem's
 * pattern (cpm_bridge.c).  If stdin isn't a TTY (e.g. pipe), fd_ioctl
 * returns an error and we leave termios_saved=0 so on_exit skips the
 * restore.
 *
 * noinline: this function allocates dos_proc_cold_t on its frame,
 * and without the attribute gets inlined into msdos_on_init whose
 * own dos_proc_t + dos_proc_cold_t locals end up sharing the same
 * stack frame — wasting ~60 B that the exec-time stack can't afford. */
__attribute__((noinline)) static void dos_tty_enter_raw(struct pcb *p) {
  int16_t desc = p->fd_map[0];
  if (desc < 0) return;

  struct termios tio;
  int rc = mod_vfs.fd_ioctl(desc, TCGETS, &tio);
  if (rc < 0) return;

  dos_proc_cold_t cold;
  dos_get_cold(p, &cold);
  cold.saved_c_iflag = tio.c_iflag;
  cold.saved_c_oflag = tio.c_oflag;
  cold.saved_c_cflag = tio.c_cflag;
  cold.saved_c_lflag = tio.c_lflag;
  cold.saved_c_line = tio.c_line;
  __builtin_memcpy(cold.saved_c_cc, tio.c_cc, sizeof(cold.saved_c_cc));
  cold.termios_saved = 1;
  dos_put_cold(p, &cold);

  tio.c_iflag &= ~(uint32_t)ICRNL;
  tio.c_lflag &= ~(uint32_t)(ICANON | ECHO);
  mod_vfs.fd_ioctl(desc, TCSETS, &tio);
}

__attribute__((noinline)) static void dos_tty_restore(struct pcb *p) {
  dos_proc_cold_t cold;
  dos_get_cold(p, &cold);
  if (!cold.termios_saved) return;
  int16_t desc = p->fd_map[0];
  if (desc < 0) return;

  struct termios tio;
  tio.c_iflag = cold.saved_c_iflag;
  tio.c_oflag = cold.saved_c_oflag;
  tio.c_cflag = cold.saved_c_cflag;
  tio.c_lflag = cold.saved_c_lflag;
  tio.c_line = cold.saved_c_line;
  __builtin_memcpy(tio.c_cc, cold.saved_c_cc, sizeof(tio.c_cc));
  mod_vfs.fd_ioctl(desc, TCSETS, &tio);
  cold.termios_saved = 0;
  dos_put_cold(p, &cold);
}

static void msdos_on_init(struct pcb *p) {
  if (dos_data_page == PAGE_ID_INVALID) {
    dos_data_page = mem_region_page_alloc();
    if (dos_data_page == PAGE_ID_INVALID) return;
    mem_region_page_zero(dos_data_page, 0, PAGE_SIZE);
  }

  dos_proc_t dos;
  __builtin_memset(&dos, 0, sizeof(dos));
  for (int i = 0; i < DOS_MAX_HANDLES; i++) dos.handle_to_fd[i] = -1;
  /* Pre-open standard handles */
  dos.handle_to_fd[0] = 0; /* stdin */
  dos.handle_to_fd[1] = 1; /* stdout */
  dos.handle_to_fd[2] = 2; /* stderr */
  dos.handle_to_fd[3] = 2; /* stdaux -> stderr */
  dos.handle_to_fd[4] = 2; /* stdprn -> stderr */
  dos.cpu_state = p->cpu_state;
  dos.current_drive = 2; /* C: — exec_dir of the running .COM */

  /* Default DTA = PSP:0080 per DOS convention.  With psp_seg=0 that
   * maps to linear 0x80 (BDA); AH=1Ah overrides it on first call. */
  dos.dta_seg = 0;
  dos.dta_off = 0x0080u;
  dos.find_fd = -1;

  dos_put_proc(p, &dos);
  /* Zero the paths slot (exec_dir / cwd_c / cwd_z) so a stale prior
   * DOS tenant doesn't leak in.  dos_set_exec_dir overwrites
   * exec_dir right after this for C:, cwd_* start empty. */
  mem_region_page_zero(dos_data_page, dos_paths_base(p),
                       (uint16_t)sizeof(dos_proc_paths_t));
  /* Zero the cold slot (IVT saves / termios snapshot). */
  dos_proc_cold_t cold;
  __builtin_memset(&cold, 0, sizeof(cold));
  dos_put_cold(p, &cold);

  dos_tty_enter_raw(p);

  p->subsys_data = (void *)(uintptr_t)(p - proc_table); /* Store slot index */
}

/* Restore any real-IVT vectors that AH=25h overwrote while this process
 * was running.  See dos_set_int_vector() for the capture side. */
static void msdos_on_exit(struct pcb *p) {
  if (dos_data_page != PAGE_ID_INVALID) {
    dos_proc_t dos;
    dos_get_proc(p, &dos);
    dos_proc_cold_t cold;
    dos_get_cold(p, &cold);
    for (int i = 0; i < cold.ivt_saved_count; i++) {
      uint32_t addr = (uint32_t)cold.ivt_saved_vec[i] * 4;
      p->cpu_ops->write16(dos.cpu_state, addr, cold.ivt_saved_ip[i]);
      p->cpu_ops->write16(dos.cpu_state, addr + 2, cold.ivt_saved_cs[i]);
    }
    cold.ivt_saved_count = 0;
    dos_put_cold(p, &cold);
    if (dos.find_fd >= 0) {
      sys_close(dos.find_fd);
      dos.find_fd = -1;
    }
    dos_put_proc(p, &dos);
    dos_tty_restore(p);
  }
  p->subsys_data = NULL;
}

/* ── Exec-path capture ──────────────────────────────────────────────── */

void dos_set_exec_dir(struct pcb *p, const struct exec_args *args) {
  if (!p || !args || dos_data_page == PAGE_ID_INVALID) return;
  if (args->argc == 0) return;

  /* Find the offset of the last '/' in argv[0], scanning byte-by-byte
   * out of the args page so no full-path stack buffer is needed. */
  uint16_t plen = exec_args_argv_len(args, 0);
  uint16_t slash = (uint16_t)0xFFFFu;
  for (uint16_t i = 0; i < plen; i++) {
    char c;
    if (exec_args_argv_byte(args, 0, i, &c) < 0) break;
    if (c == '/') slash = i;
  }

  /* Write exec_dir byte-by-byte into the cold slot on dos_data_page. */
  uint16_t base = (uint16_t)(dos_paths_base(p) + DOS_EXEC_DIR_FIELD_OFF);
  int n = 0;
  if (slash == 0xFFFFu || slash == 0) {
    dos_scratch_putb((uint16_t)(base + n), '/');
    n++;
  } else {
    int len = (int)slash;
    if (len >= DOS_PATH_MAX) len = DOS_PATH_MAX - 1;
    for (int i = 0; i < len; i++) {
      char c;
      if (exec_args_argv_byte(args, 0, (uint16_t)i, &c) < 0) break;
      dos_scratch_putb((uint16_t)(base + n), (uint8_t)c);
      n++;
    }
  }
  dos_scratch_putb((uint16_t)(base + n), 0);
}

/* ── Path resolution ────────────────────────────────────────────────── *
 *
 * Stage the translated PPAP path into a kmem-backed scratch slot
 * (DOS_PATH_SCRATCH_OFF inside dos_data_page) and call sys_open with
 * that (page, off) pair.  The user-pointer translator runs only
 * at the syscall dispatcher boundary — kernel callers like this one
 * supply a page reference directly and don't have to mimic a user
 * pointer.
 *
 * Bytes are streamed directly into DOS_PATH_SCRATCH_OFF via
 * dos_scratch_putb — no intermediate stack buffer, so the kernel-stack
 * footprint stays tiny. */

static int dos_resolve_user_path(dos_proc_t *dos, uint16_t in_seg,
                                 uint16_t in_off) {
  if (dos_data_page == PAGE_ID_INVALID) return -DOS_ERR_PATH_NOT_FOUND;

  uint8_t drive = dos->current_drive;

  /* Drive letter?  "X:" at the start, where X is a letter. */
  uint8_t c0 = (uint8_t)current->cpu_ops->read8(dos->cpu_state,
                                                dos_to_linear(in_seg, in_off));
  if (c0 && c0 != '\\' && c0 != '/') {
    uint8_t c1 = (uint8_t)current->cpu_ops->read8(
        dos->cpu_state, dos_to_linear(in_seg, (uint16_t)(in_off + 1)));
    if (c1 == ':') {
      char d = (char)c0;
      if (d >= 'a' && d <= 'z') d = (char)(d - ('a' - 'A'));
      if (d == 'C')
        drive = 2;
      else if (d == 'Z')
        drive = 25;
      else
        return -DOS_ERR_INVALID_DRIVE;
      in_off += 2;
    }
  }

  uint16_t paths_base = dos_paths_base(current);
  uint16_t cwd_off;
  if (drive == 2) {
    cwd_off = (uint16_t)(paths_base + DOS_CWD_C_FIELD_OFF);
  } else if (drive == 25) {
    cwd_off = (uint16_t)(paths_base + DOS_CWD_Z_FIELD_OFF);
  } else {
    return -DOS_ERR_INVALID_DRIVE;
  }

  uint8_t peek = (uint8_t)current->cpu_ops->read8(
      dos->cpu_state, dos_to_linear(in_seg, in_off));
  int absolute = (peek == '/' || peek == '\\');
  if (absolute) in_off++;

  uint16_t n = 0;

  /* Copy root, dropping a trailing '/' so we never emit '//'.  For
   * drive 2 the root is exec_dir from the paths slot; for drive 25
   * the root is the literal "/" which we skip here — the n==0
   * fallback later supplies the leading slash. */
  if (drive == 2) {
    uint16_t root_off = (uint16_t)(paths_base + DOS_EXEC_DIR_FIELD_OFF);
    uint16_t root_len = 0;
    for (; root_len < DOS_PATH_MAX; root_len++) {
      if (dos_scratch_getb((uint16_t)(root_off + root_len)) == 0) break;
    }
    if (root_len > 0 &&
        dos_scratch_getb((uint16_t)(root_off + root_len - 1)) == '/') {
      root_len--;
    }
    for (uint16_t i = 0; i < root_len; i++) {
      if (n >= DOS_PATH_SCRATCH_MAX - 1) return -DOS_ERR_PATH_NOT_FOUND;
      dos_scratch_putb((uint16_t)(DOS_PATH_SCRATCH_OFF + n),
                       dos_scratch_getb((uint16_t)(root_off + i)));
      n++;
    }
  }

  /* Copy cwd from the cold slot (relative path only). */
  if (!absolute && dos_scratch_getb(cwd_off) != 0) {
    if (n >= DOS_PATH_SCRATCH_MAX - 1) return -DOS_ERR_PATH_NOT_FOUND;
    dos_scratch_putb((uint16_t)(DOS_PATH_SCRATCH_OFF + n), '/');
    n++;
    for (uint16_t i = 0; i < DOS_PATH_MAX; i++) {
      uint8_t b = dos_scratch_getb((uint16_t)(cwd_off + i));
      if (b == 0) break;
      if (n >= DOS_PATH_SCRATCH_MAX - 1) return -DOS_ERR_PATH_NOT_FOUND;
      dos_scratch_putb((uint16_t)(DOS_PATH_SCRATCH_OFF + n), b);
      n++;
    }
  }

  peek = (uint8_t)current->cpu_ops->read8(dos->cpu_state,
                                          dos_to_linear(in_seg, in_off));
  if (peek) {
    if (n >= DOS_PATH_SCRATCH_MAX - 1) return -DOS_ERR_PATH_NOT_FOUND;
    dos_scratch_putb((uint16_t)(DOS_PATH_SCRATCH_OFF + n), '/');
    n++;
    for (;;) {
      uint8_t b = (uint8_t)current->cpu_ops->read8(
          dos->cpu_state, dos_to_linear(in_seg, in_off++));
      if (b == 0) break;
      if (b == '\\') b = '/';
      if (n >= DOS_PATH_SCRATCH_MAX - 1) return -DOS_ERR_PATH_NOT_FOUND;
      dos_scratch_putb((uint16_t)(DOS_PATH_SCRATCH_OFF + n), b);
      n++;
    }
  } else if (n == 0) {
    dos_scratch_putb((uint16_t)DOS_PATH_SCRATCH_OFF, '/');
    n++;
  }
  dos_scratch_putb((uint16_t)(DOS_PATH_SCRATCH_OFF + n), 0);
  return 0;
}

/* ── Handle table ───────────────────────────────────────────────────── */

static int dos_alloc_handle(dos_proc_t *dos, int fd) {
  for (int h = DOS_FIRST_USER_HANDLE; h < DOS_MAX_HANDLES; h++) {
    if (dos->handle_to_fd[h] < 0) {
      dos->handle_to_fd[h] = fd;
      return h;
    }
  }
  return -DOS_ERR_TOO_MANY_OPEN;
}

static int dos_lookup_fd(const dos_proc_t *dos, int handle) {
  if (handle < 0 || handle >= DOS_MAX_HANDLES) return -DOS_ERR_INVALID_HANDLE;
  int fd = dos->handle_to_fd[handle];
  return (fd < 0) ? -DOS_ERR_INVALID_HANDLE : fd;
}

/* ── errno → DOS error ──────────────────────────────────────────────── */

static int dos_errno_to_dos(int err) {
  if (err < 0) err = -err;
  switch (err) {
    case ENOENT:
      return DOS_ERR_FILE_NOT_FOUND;
    case ENOTDIR:
      return DOS_ERR_PATH_NOT_FOUND;
    case EMFILE:
      return DOS_ERR_TOO_MANY_OPEN;
    case EPERM:
    case EACCES:
    case EROFS:
    case EISDIR:
      return DOS_ERR_ACCESS_DENIED;
    case EBADF:
      return DOS_ERR_INVALID_HANDLE;
    case ENOMEM:
      return DOS_ERR_INSUFFICIENT_MEMORY;
    case EEXIST:
      return DOS_ERR_FILE_EXISTS;
    default:
      return DOS_ERR_INVALID_FUNCTION;
  }
}

static int msdos_on_proc_read(struct pcb *p, const char *name, char *buf,
                              int bufsiz) {
  return -1;
}

const subsys_ops_t msdos_subsys_ops = {
    .on_crash = msdos_on_crash,
    .on_signal = msdos_on_signal,
    .on_init = msdos_on_init,
    .on_exit = msdos_on_exit,
    .on_proc_read = msdos_on_proc_read,
};

/* ── Single-byte fd I/O helpers ────────────────────────────────────────
 *
 * Stage single bytes through DOS_IO_SCRATCH_OFF in the kmem-backed
 * dos_data_page and use sys_*_pf to hand the (page, off) pair to the
 * VFS layer directly — no per-arch user-pointer translation.  Same
 * single implementation works on ia16 and flat archs. */

static long dos_io_putc(uint8_t c) {
  if (dos_data_page == PAGE_ID_INVALID) return -1;
  mem_region_page_write(dos_data_page, DOS_IO_SCRATCH_OFF, &c, 1);
  return sys_write(1, dos_data_page, DOS_IO_SCRATCH_OFF, 1);
}

static long dos_io_getc(uint8_t *out) {
  if (dos_data_page == PAGE_ID_INVALID) return -1;
  /* DOS has no concept of -EINTR; a signal arriving during a blocking
   * console read just retries.  Any other negative status (e.g. EOF=0)
   * is returned to the caller. */
  for (;;) {
    long n = sys_read(0, dos_data_page, DOS_IO_SCRATCH_OFF, 1);
    if (n == -(long)EINTR) continue;
    if (n != 1) return n;
    mem_region_page_read(dos_data_page, DOS_IO_SCRATCH_OFF, out, 1);
    return 1;
  }
}

/* Non-blocking stdin probe used by AH=0Bh.  Returns 1 if a byte is
 * available (either already stashed or just read) and leaves it
 * stashed in dos->stdin_pushback_char for a later blocking-style read
 * to drain.  Returns 0 if no byte is available. */
static int dos_stdin_peek(dos_proc_t *dos) {
  if (dos->stdin_pushback_valid) return 1;
  if (dos_data_page == PAGE_ID_INVALID) return 0;

  long flags = sys_fcntl64(0, F_GETFL, 0);
  if (flags < 0) flags = 0;
  sys_fcntl64(0, F_SETFL, flags | O_NONBLOCK);
  long n = sys_read(0, dos_data_page, DOS_IO_SCRATCH_OFF, 1);
  sys_fcntl64(0, F_SETFL, flags);

  if (n == 1) {
    uint8_t c;
    mem_region_page_read(dos_data_page, DOS_IO_SCRATCH_OFF, &c, 1);
    dos->stdin_pushback_char = c;
    dos->stdin_pushback_valid = 1;
    return 1;
  }
  return 0;
}

/* Drain-first blocking getc.  If AH=0Bh previously stashed a byte,
 * consume it; otherwise fall back to dos_io_getc's blocking read. */
static long dos_stdin_getc(dos_proc_t *dos, uint8_t *out) {
  if (dos->stdin_pushback_valid) {
    *out = dos->stdin_pushback_char;
    dos->stdin_pushback_valid = 0;
    return 1;
  }
  return dos_io_getc(out);
}

/* ── INT 21h functions ─────────────────────────────────────────────── */

static int dos_read_char(dos_proc_t *dos, dos_regs_t *regs, int echo) {
  uint8_t c;
  if (dos_stdin_getc(dos, &c) == 1) {
    if (echo) dos_io_putc(c);
    regs->ax = (regs->ax & 0xFF00) | c;
    return 0;
  }
  return -1;
}

static int dos_write_char(dos_proc_t *dos, dos_regs_t *regs) {
  dos_io_putc((uint8_t)(regs->dx & 0xFF));
  return 0;
}

static int dos_print_string(dos_proc_t *dos, dos_regs_t *regs) {
  uint16_t off = regs->dx;
  for (;;) {
    uint8_t c = (uint8_t)current->cpu_ops->read8(
        dos->cpu_state, dos_to_linear(regs->ds, off++));
    if (c == '$') break;
    dos_io_putc(c);
  }
  return 0;
}

static int dos_buffered_input(dos_proc_t *dos, dos_regs_t *regs) {
  uint16_t seg = regs->ds;
  uint16_t off = regs->dx;
  uint8_t max_len =
      current->cpu_ops->read8(dos->cpu_state, dos_to_linear(seg, off));
  uint8_t actual_len = 0;

  for (actual_len = 0; actual_len < max_len - 1; actual_len++) {
    uint8_t c;
    if (dos_stdin_getc(dos, &c) != 1) break;
    if (c == '\r' || c == '\n') break;
    dos_io_putc(c);
    current->cpu_ops->write8(dos->cpu_state,
                             dos_to_linear(seg, off + 2 + actual_len), c);
  }
  current->cpu_ops->write8(dos->cpu_state, dos_to_linear(seg, off + 1),
                           actual_len);
  current->cpu_ops->write8(dos->cpu_state,
                           dos_to_linear(seg, off + 2 + actual_len), '\r');
  dos_io_putc('\n');

  return 0;
}

/* AH=06h — Direct Console I/O.
 *   DL ≠ FFh: write DL to stdout, AL = DL.
 *   DL = FFh: non-blocking read from stdin.  ZF=0 with AL=byte if a
 *             character was available; ZF=1 with AL=0 otherwise.
 *
 * Implements the non-blocking read POSIX-style: flip O_NONBLOCK on
 * fd 0 around a single sys_read so the underlying TTY returns
 * -EAGAIN instead of blocking, then restore the original flags.
 * On -EAGAIN we yield before returning "no char" so the typical
 * poll-in-a-tight-loop DOS app doesn't starve the rest of the system. */
static int dos_direct_console_io(dos_proc_t *dos, dos_regs_t *regs) {
  uint8_t dl = (uint8_t)(regs->dx & 0xFF);
  if (dl != 0xFF) {
    dos_io_putc(dl);
    regs->ax = (regs->ax & 0xFF00) | dl;
    return 0;
  }

  if (dos_stdin_peek(dos)) {
    /* Drain the stashed byte (from this peek or a prior AH=0Bh). */
    uint8_t c = dos->stdin_pushback_char;
    dos->stdin_pushback_valid = 0;
    regs->ax = (regs->ax & 0xFF00) | c;
    regs->flags &= ~0x0040u; /* ZF = 0: char available */
  } else {
    sched_switch();
    regs->ax &= 0xFF00u;    /* AL = 0 */
    regs->flags |= 0x0040u; /* ZF = 1: no char */
  }
  return 0;
}

static int dos_check_input_status(dos_proc_t *dos, dos_regs_t *regs) {
  /* AL=FFh iff a byte is actually available on stdin.  On "no char"
   * yield so the typical poll-in-a-tight-loop DOS app doesn't starve
   * the rest of the system. */
  if (dos_stdin_peek(dos)) {
    regs->ax = (regs->ax & 0xFF00) | 0xFF;
  } else {
    sched_switch();
    regs->ax = regs->ax & 0xFF00;
  }
  return 0;
}

static int dos_get_current_drive(dos_proc_t *dos, dos_regs_t *regs) {
  regs->ax = (regs->ax & 0xFF00) | dos->current_drive;
  return 0;
}

/* Convert Unix epoch days to (year, month [1-12], day [1-31]) and return
 * day-of-week where 0 = Sunday (matching DOS AH=2Ah convention).  1970-
 * 01-01 was a Thursday. */
static void dos_days_to_ymd(uint32_t days, uint16_t *y, uint8_t *mo, uint8_t *d,
                            uint8_t *dow) {
  static const uint8_t dim_nl[12] = {31, 28, 31, 30, 31, 30,
                                     31, 31, 30, 31, 30, 31};
  static const uint8_t dim_l[12] = {31, 29, 31, 30, 31, 30,
                                    31, 31, 30, 31, 30, 31};
  *dow = (uint8_t)((days + 4u) % 7u); /* 1970-01-01 = Thu (4) */
  uint16_t year = 1970;
  for (;;) {
    int leap = ((year % 4u) == 0 && (year % 100u) != 0) || (year % 400u) == 0;
    uint16_t year_days = leap ? 366 : 365;
    if (days < year_days) break;
    days -= year_days;
    year++;
  }
  int leap = ((year % 4u) == 0 && (year % 100u) != 0) || (year % 400u) == 0;
  const uint8_t *dim = leap ? dim_l : dim_nl;
  uint8_t month = 0;
  while (days >= dim[month]) {
    days -= dim[month];
    month++;
  }
  *y = year;
  *mo = (uint8_t)(month + 1u);
  *d = (uint8_t)(days + 1u);
}

static int dos_get_date(dos_proc_t *dos, dos_regs_t *regs) {
  (void)dos;
  uint32_t sec, frac;
  sys_time_now(&sec, &frac);
  uint32_t days = sec / 86400u;
  uint16_t y;
  uint8_t mo, d, dow;
  dos_days_to_ymd(days, &y, &mo, &d, &dow);
  regs->cx = y;
  regs->dx = (uint16_t)(((uint16_t)mo << 8) | d);
  regs->ax = (uint16_t)((regs->ax & 0xFF00u) | dow);
  return 0;
}

static int dos_get_time(dos_proc_t *dos, dos_regs_t *regs) {
  (void)dos;
  uint32_t sec, frac;
  sys_time_now(&sec, &frac);
  uint32_t sub = (frac * 100u) / PPAP_TICK_HZ; /* hundredths of second */
  uint32_t s_in_day = sec % 86400u;
  uint32_t ss = s_in_day % 60u;
  uint32_t mm = (s_in_day / 60u) % 60u;
  uint32_t hh = (s_in_day / 3600u) % 24u;
  regs->cx = (uint16_t)((hh << 8) | mm);
  regs->dx = (uint16_t)((ss << 8) | sub);
  return 0;
}

static int dos_get_version(dos_proc_t *dos, dos_regs_t *regs) {
  regs->ax = 0x1E03; /* DOS 3.30 (AL=major, AH=minor) */
  regs->bx = 0x0000; /* Serial number */
  regs->cx = 0x0000;
  return 0;
}

/* AH=1Ah Set Disk Transfer Address — DS:DX = new DTA.  Stored for
 * AH=2Fh readback and for AH=4Eh/4Fh FindFirst/FindNext to write the
 * 128-byte search result frame into. */
static int dos_set_dta(dos_proc_t *dos, dos_regs_t *regs) {
  dos->dta_seg = regs->ds;
  dos->dta_off = regs->dx;
  return 0;
}

/* AH=2Fh Get Disk Transfer Address — ES:BX = current DTA. */
static int dos_get_dta(dos_proc_t *dos, dos_regs_t *regs) {
  regs->es = dos->dta_seg;
  regs->bx = dos->dta_off;
  return 0;
}

/* ── FindFirst / FindNext helpers ───────────────────────────────────── */

static uint8_t dos_toupper8(uint8_t c) {
  return (c >= 'a' && c <= 'z') ? (uint8_t)(c - ('a' - 'A')) : c;
}

/* DOS 8.3 glob matcher — `*` matches zero or more, `?` matches exactly
 * one, comparison is case-insensitive.  Recursion depth is bounded by
 * the pattern length (max 12), so the extra stack is tiny. */
static int dos_glob_match(const uint8_t *pat, const char *name) {
  while (*pat || *name) {
    if (*pat == '*') {
      pat++;
      if (!*pat) return 1;
      while (*name) {
        if (dos_glob_match(pat, name)) return 1;
        name++;
      }
      return 0;
    }
    if (!*pat) return 0;
    if (*pat != '?' && *pat != dos_toupper8((uint8_t)*name)) return 0;
    pat++;
    name++;
  }
  return 1;
}

/* Split the path currently sitting in DOS_PATH_SCRATCH_OFF into a
 * directory portion (left in the scratch, null-terminated at the last
 * '/') and an uppercase pattern (copied into `pat_out`, bounded by
 * pat_max).  Empty patterns and paths without a '/' fail with
 * PATH_NOT_FOUND.  Root-level patterns like "/pat" leave "/" in the
 * scratch so sys_open opens the root dir. */
static int dos_find_split_scratch(uint8_t *pat_out, uint16_t pat_max) {
  /* First pass: scan the page for NUL and track the last '/' index. */
  uint16_t path_len = 0;
  int slash_idx = -1;
  for (uint16_t i = 0; i < DOS_PATH_SCRATCH_MAX; i++) {
    uint8_t b = dos_scratch_getb((uint16_t)(DOS_PATH_SCRATCH_OFF + i));
    if (b == 0) {
      path_len = i;
      break;
    }
    if (b == '/') slash_idx = (int)i;
  }
  if (path_len == 0 || slash_idx < 0) return -DOS_ERR_PATH_NOT_FOUND;

  uint16_t pat_start = (uint16_t)(slash_idx + 1);
  if (pat_start >= path_len) return -DOS_ERR_PATH_NOT_FOUND;

  /* Second pass: copy the pattern out, uppercasing in flight. */
  uint16_t p_i = 0;
  for (uint16_t i = pat_start; i < path_len && p_i + 1 < pat_max; i++) {
    uint8_t b = dos_scratch_getb((uint16_t)(DOS_PATH_SCRATCH_OFF + i));
    pat_out[p_i++] = dos_toupper8(b);
  }
  pat_out[p_i] = 0;

  /* Truncate the scratch path at the last '/' so sys_open opens the
   * directory.  dir_len==0 means the match is at root — leave "/" in
   * place so sys_open sees a non-empty path. */
  uint16_t dir_len = (uint16_t)slash_idx;
  uint16_t term_off = (dir_len == 0)
                          ? (uint16_t)(DOS_PATH_SCRATCH_OFF + 1)
                          : (uint16_t)(DOS_PATH_SCRATCH_OFF + dir_len);
  dos_scratch_putb(term_off, 0);
  return 0;
}

/* Pack Unix epoch seconds into the DOS date + time word format used
 * in the DTA for AH=4Eh/4Fh.  Dates before 1980 (the DOS epoch) are
 * clamped to zero — real DOS reports them the same way. */
static void dos_pack_mtime(uint32_t mtime, uint16_t *dos_date,
                           uint16_t *dos_time) {
  uint32_t days = mtime / 86400u;
  uint32_t s_in_day = mtime % 86400u;
  uint16_t y;
  uint8_t mo, d, dow;
  dos_days_to_ymd(days, &y, &mo, &d, &dow);
  if (y < 1980) {
    *dos_date = 0;
    *dos_time = 0;
    return;
  }
  *dos_date = (uint16_t)(((y - 1980u) << 9) | ((uint32_t)mo << 5) | d);
  uint32_t hh = s_in_day / 3600u;
  uint32_t mm = (s_in_day / 60u) % 60u;
  uint32_t ss = s_in_day % 60u;
  *dos_time = (uint16_t)((hh << 11) | (mm << 5) | (ss >> 1));
}

/* Write the 128-byte DTA result frame for a matched entry.  PPAP's
 * filenames can exceed the DTA's 13-byte slot; truncate at 12 + null,
 * which is lossy for long names. */
static void dos_find_fill_dta(dos_proc_t *dos, uint8_t attr, uint32_t size,
                              uint16_t dos_date, uint16_t dos_time,
                              const char *name) {
  uint32_t base = ((uint32_t)dos->dta_seg << 4) + dos->dta_off;

  /* Reserved 0-20: zero. */
  for (uint16_t i = 0; i < 21; i++) {
    current->cpu_ops->write8(dos->cpu_state, base + i, 0);
  }
  current->cpu_ops->write8(dos->cpu_state, base + 21, attr);
  current->cpu_ops->write16(dos->cpu_state, base + 22, dos_time);
  current->cpu_ops->write16(dos->cpu_state, base + 24, dos_date);
  current->cpu_ops->write16(dos->cpu_state, base + 26,
                            (uint16_t)(size & 0xFFFFu));
  current->cpu_ops->write16(dos->cpu_state, base + 28,
                            (uint16_t)((size >> 16) & 0xFFFFu));

  uint16_t n = 0;
  while (n < 12 && name[n]) {
    current->cpu_ops->write8(dos->cpu_state, base + 30 + n, (uint8_t)name[n]);
    n++;
  }
  while (n < 13) {
    current->cpu_ops->write8(dos->cpu_state, base + 30 + n, 0);
    n++;
  }
}

/* Stat the matched entry and return raw size + Unix-epoch mtime.
 * dir path is still in DOS_PATH_SCRATCH_OFF from the split step and
 * survives across scan calls as long as the app does not issue
 * another DOS path operation between FindFirst and FindNext.  Best-
 * effort: if composing the full path would overflow scratch, or the
 * lookup / stat fails, *size_out and *mtime_out stay whatever the
 * caller initialised them to (usually zero).  Callers pack mtime
 * into the DOS or WIN32 time format they need after the call.
 *
 * noinline keeps the 128-byte path stack copy out of the find-scan
 * caller's frame on every dirent. */
static int __attribute__((noinline)) dos_stat_entry(const char *name,
                                                    uint32_t *size_out,
                                                    uint32_t *mtime_out) {
  /* Compose "<dir>/<name>" in the scratch page at DOS_STAT_FULL_OFF —
   * no stack buffer.  The dir prefix is already sitting at
   * DOS_PATH_SCRATCH_OFF from the preceding split step, so we stream
   * it byte-by-byte into DOS_STAT_FULL_OFF and track its length and
   * first byte in passing. */
  uint16_t dir_len = 0;
  uint8_t first = 0;
  for (; dir_len < DOS_STAT_FULL_MAX; dir_len++) {
    uint8_t b = dos_scratch_getb((uint16_t)(DOS_PATH_SCRATCH_OFF + dir_len));
    if (!b) break;
    if (dir_len == 0) first = b;
    dos_scratch_putb((uint16_t)(DOS_STAT_FULL_OFF + dir_len), b);
  }
  uint16_t name_len = 0;
  while (name[name_len]) name_len++;
  int need_slash = (dir_len > 0 && !(dir_len == 1 && first == '/'));
  if ((uint16_t)(dir_len + (need_slash ? 1 : 0) + name_len + 1) >
      (uint16_t)DOS_STAT_FULL_MAX)
    return -1;
  uint16_t pos = dir_len;
  if (need_slash) {
    dos_scratch_putb((uint16_t)(DOS_STAT_FULL_OFF + pos), '/');
    pos++;
  }
  for (uint16_t i = 0; i < name_len; i++) {
    dos_scratch_putb((uint16_t)(DOS_STAT_FULL_OFF + pos), (uint8_t)name[i]);
    pos++;
  }
  dos_scratch_putb((uint16_t)(DOS_STAT_FULL_OFF + pos), 0);

  /* mod_vfs.lookup needs a C string; copy the page-backed path into a
   * stack buffer.  The earlier "near pointer at linear addr" trick
   * silently passed garbage — lookup happily resolved unrelated
   * memory and returned spurious success, masking missing files. */
  char path[DOS_STAT_FULL_MAX];
  for (uint16_t i = 0; i < DOS_STAT_FULL_MAX; i++) {
    uint8_t b;
    mem_region_page_read(dos_data_page, (uint16_t)(DOS_STAT_FULL_OFF + i), &b,
                         1);
    path[i] = (char)b;
    if (!b) break;
  }
  path[DOS_STAT_FULL_MAX - 1] = '\0';

  vnode_t *vn = NULL;
  if (mod_vfs.lookup(path, &vn) != 0) return -1;
  struct stat st;
  int err = mod_vfs.vnode_stat(vn, &st);
  mod_vfs.vnode_release(vn);
  if (err) return -1;
  *size_out = st.st_size;
  *mtime_out = st.st_mtime;
  return 0;
}

/* Read the next matching dirent from dos->find_fd, fill DTA, and
 * return 0.  On EOF, close the fd and return -DOS_ERR_NO_MORE_FILES
 * (FindFirst translates that to FILE_NOT_FOUND). */
static int dos_find_scan(dos_proc_t *dos) {
  if (dos->find_fd < 0) return -DOS_ERR_NO_MORE_FILES;
  int16_t desc = current->fd_map[dos->find_fd];
  if (desc < 0) {
    dos->find_fd = -1;
    return -DOS_ERR_NO_MORE_FILES;
  }

  for (;;) {
    long n = mod_vfs.fd_getdents(desc, dos_data_page,
                                 (uint16_t)DOS_DIRENT_SCRATCH_OFF,
                                 sizeof(struct dirent));
    if (n < 0) {
      sys_close(dos->find_fd);
      dos->find_fd = -1;
      return -dos_errno_to_dos((int)n);
    }
    if (n == 0) {
      sys_close(dos->find_fd);
      dos->find_fd = -1;
      return -DOS_ERR_NO_MORE_FILES;
    }

    struct dirent d;
    mem_region_page_read(dos_data_page, (uint16_t)DOS_DIRENT_SCRATCH_OFF, &d,
                         sizeof(d));

    if (!dos_glob_match(dos->find_pattern, d.d_name)) continue;

    uint8_t attr = (d.d_type == DT_DIR) ? 0x10u : 0x20u;
    uint32_t size = 0;
    uint32_t mtime = 0;
    (void)dos_stat_entry(d.d_name, &size, &mtime);
    uint16_t dos_date = 0, dos_time = 0;
    if (mtime) dos_pack_mtime(mtime, &dos_date, &dos_time);
    dos_find_fill_dta(dos, attr, size, dos_date, dos_time, d.d_name);
    return 0;
  }
}

/* AH=4Eh FindFirst — DS:DX = ASCIIZ search pattern, CX = attribute
 * mask.  PPAP ignores CX for now and reports every matching entry
 * with its own synthesised attribute (DIRECTORY or ARCHIVE). */
static int dos_find_first(dos_proc_t *dos, dos_regs_t *regs) {
  if (dos->find_fd >= 0) {
    sys_close(dos->find_fd);
    dos->find_fd = -1;
  }

  int rc = dos_resolve_user_path(dos, regs->ds, regs->dx);
  if (rc < 0) return rc;

  rc = dos_find_split_scratch(dos->find_pattern, sizeof(dos->find_pattern));
  if (rc < 0) return rc;

  long fd = sys_open(dos_data_page, DOS_PATH_SCRATCH_OFF, O_RDONLY, 0);
  if (fd < 0) return -dos_errno_to_dos((int)fd);
  dos->find_fd = (int)fd;

  rc = dos_find_scan(dos);
  if (rc == -DOS_ERR_NO_MORE_FILES) return -DOS_ERR_FILE_NOT_FOUND;
  return rc;
}

/* AH=4Fh FindNext — resume the previously-started search.  Returns
 * NO_MORE_FILES at end-of-directory, not FILE_NOT_FOUND. */
static int dos_find_next(dos_proc_t *dos, dos_regs_t *regs) {
  (void)regs;
  return dos_find_scan(dos);
}

/* ── AH=71h LFN subset — Phase L1 (TrueName only) ──────────────────── */

/* AL=60h LFN "TrueName" — DS:SI = source ASCIIZ, ES:DI = destination
 * (up to 260 bytes).  CL = 00h short / 01h long / 02h canonical; PPAP
 * has no SFN/LFN distinction so every CL returns the same canonical
 * path.  Shape-wise we emit the VFS-form absolute path that
 * dos_resolve_user_path produces (leading slash, forward slashes)
 * rather than the DOS-form "C:\FOO\BAR" — apps that parse the reply
 * for a drive letter will need to adapt until a later phase wraps
 * the result. */
static int dos_lfn_truename(dos_proc_t *dos, dos_regs_t *regs) {
  int rc = dos_resolve_user_path(dos, regs->ds, regs->si);
  if (rc < 0) return rc;

  uint32_t dst_linear = ((uint32_t)regs->es << 4) + regs->di;
  for (uint16_t i = 0; i < DOS_PATH_SCRATCH_MAX; i++) {
    uint8_t b;
    mem_region_page_read(dos_data_page, (uint16_t)(DOS_PATH_SCRATCH_OFF + i),
                         &b, 1);
    current->cpu_ops->write8(dos->cpu_state, dst_linear + i, b);
    if (b == 0) return 0;
  }
  /* Resolved path exceeded scratch; emit terminator and fail. */
  current->cpu_ops->write8(dos->cpu_state,
                           dst_linear + DOS_PATH_SCRATCH_MAX - 1, 0);
  return -DOS_ERR_PATH_NOT_FOUND;
}

/* Write the 318-byte WIN32_FIND_DATA result frame at ES:DI for an
 * LFN FindFirst / FindNext match.  date_fmt selects the time
 * encoding: 0 = Windows FILETIME (100-ns intervals since 1601-01-01),
 * 1 = DOS date/time word pair packed into the low 4 bytes with the
 * upper 4 bytes zero.  PPAP has no short-name alias so
 * cAlternateFileName[] is emitted as a zero-terminated empty string.
 * cFileName[] carries the full long name (up to 259 chars + NUL). */
static void dos_find_fill_lfn(dos_proc_t *dos, uint16_t es, uint16_t di,
                              uint8_t date_fmt, uint8_t dos_attr, uint32_t size,
                              uint32_t mtime, const char *name) {
  uint32_t base = ((uint32_t)es << 4) + di;

  /* 0x00 dwFileAttributes.  FILE_ATTRIBUTE_DIRECTORY=0x10,
   * FILE_ATTRIBUTE_ARCHIVE=0x20; happens to overlap DOS bits. */
  uint32_t win_attr = (uint32_t)dos_attr;
  for (int i = 0; i < 4; i++)
    current->cpu_ops->write8(dos->cpu_state, base + i,
                             (uint8_t)(win_attr >> (i * 8)));

  /* Build the FILETIME field once and duplicate it for ctime / atime
   * / mtime.  PPAP only tracks mtime so the three slots report the
   * same instant. */
  uint8_t ft[8] = {0};
  if (date_fmt == 1) {
    uint16_t dos_date = 0, dos_time = 0;
    if (mtime) dos_pack_mtime(mtime, &dos_date, &dos_time);
    ft[0] = (uint8_t)(dos_time & 0xFFu);
    ft[1] = (uint8_t)(dos_time >> 8);
    ft[2] = (uint8_t)(dos_date & 0xFFu);
    ft[3] = (uint8_t)(dos_date >> 8);
    /* upper 4 bytes zero (already) */
  } else if (mtime) {
    /* Windows FILETIME = 100-ns ticks since 1601-01-01.  Unix epoch
     * is 1970-01-01; the offset is 11644473600 seconds. */
    uint64_t unix_ns = ((uint64_t)mtime + 11644473600ULL) * 10000000ULL;
    for (int i = 0; i < 8; i++) ft[i] = (uint8_t)(unix_ns >> (i * 8));
  }
  for (int slot = 0; slot < 3; slot++) {
    uint32_t off = 4 + (uint32_t)slot * 8;
    for (int i = 0; i < 8; i++)
      current->cpu_ops->write8(dos->cpu_state, base + off + i, ft[i]);
  }

  /* 0x1C nFileSizeHigh (always zero — our sizes fit in 32-bit),
   * 0x20 nFileSizeLow. */
  for (int i = 0; i < 4; i++)
    current->cpu_ops->write8(dos->cpu_state, base + 0x1C + i, 0);
  for (int i = 0; i < 4; i++)
    current->cpu_ops->write8(dos->cpu_state, base + 0x20 + i,
                             (uint8_t)(size >> (i * 8)));

  /* 0x24 dwReserved0, 0x28 dwReserved1 */
  for (int i = 0; i < 8; i++)
    current->cpu_ops->write8(dos->cpu_state, base + 0x24 + i, 0);

  /* 0x2C cFileName[260] — full long name, NUL-terminated. */
  uint16_t n = 0;
  while (n < 259 && name[n]) {
    current->cpu_ops->write8(dos->cpu_state, base + 0x2C + n, (uint8_t)name[n]);
    n++;
  }
  while (n < 260) {
    current->cpu_ops->write8(dos->cpu_state, base + 0x2C + n, 0);
    n++;
  }

  /* 0x130 cAlternateFileName[14] — zeroed (no short-name alias). */
  for (int i = 0; i < 14; i++)
    current->cpu_ops->write8(dos->cpu_state, base + 0x130 + i, 0);
}

/* Read the next matching dirent from dos->find_fd and write a
 * WIN32_FIND_DATA frame to regs->es:regs->di.  Returns 0 on match,
 * -DOS_ERR_NO_MORE_FILES on EOF, other negative -DOS_ERR on I/O
 * error.  Uses regs->si as the date_fmt selector (0 = FILETIME,
 * 1 = DOS). */
static int dos_lfn_find_scan(dos_proc_t *dos, dos_regs_t *regs) {
  if (dos->find_fd < 0) return -DOS_ERR_NO_MORE_FILES;
  int16_t desc = current->fd_map[dos->find_fd];
  if (desc < 0) {
    dos->find_fd = -1;
    return -DOS_ERR_NO_MORE_FILES;
  }

  for (;;) {
    long n = mod_vfs.fd_getdents(desc, dos_data_page,
                                 (uint16_t)DOS_DIRENT_SCRATCH_OFF,
                                 sizeof(struct dirent));
    if (n < 0) {
      sys_close(dos->find_fd);
      dos->find_fd = -1;
      return -dos_errno_to_dos((int)n);
    }
    if (n == 0) {
      sys_close(dos->find_fd);
      dos->find_fd = -1;
      return -DOS_ERR_NO_MORE_FILES;
    }

    struct dirent d;
    mem_region_page_read(dos_data_page, (uint16_t)DOS_DIRENT_SCRATCH_OFF, &d,
                         sizeof(d));

    if (!dos_glob_match(dos->find_pattern, d.d_name)) continue;

    uint8_t attr = (d.d_type == DT_DIR) ? 0x10u : 0x20u;
    uint32_t size = 0;
    uint32_t mtime = 0;
    (void)dos_stat_entry(d.d_name, &size, &mtime);
    dos_find_fill_lfn(dos, regs->es, regs->di, (uint8_t)(regs->si & 0xFFu),
                      attr, size, mtime, d.d_name);
    return 0;
  }
}

/* AH=71h AL=4Eh LFN FindFirst — DS:DX = pattern, ES:DI = 318-byte
 * result buffer, CL/CH = attribute match masks (ignored today),
 * SI = date format (0 FILETIME / 1 DOS).  Returns AX = find handle
 * (always 1 since PPAP supports only a single concurrent find per
 * process — multi-handle support deferred). */
static int dos_lfn_find_first(dos_proc_t *dos, dos_regs_t *regs) {
  if (dos->find_fd >= 0) {
    sys_close(dos->find_fd);
    dos->find_fd = -1;
  }

  int rc = dos_resolve_user_path(dos, regs->ds, regs->dx);
  if (rc < 0) return rc;

  rc = dos_find_split_scratch(dos->find_pattern, sizeof(dos->find_pattern));
  if (rc < 0) return rc;

  long fd = sys_open(dos_data_page, DOS_PATH_SCRATCH_OFF, O_RDONLY, 0);
  if (fd < 0) return -dos_errno_to_dos((int)fd);
  dos->find_fd = (int)fd;

  rc = dos_lfn_find_scan(dos, regs);
  if (rc == -DOS_ERR_NO_MORE_FILES) return -DOS_ERR_FILE_NOT_FOUND;
  if (rc < 0) return rc;
  regs->ax = 1; /* dummy find handle */
  return 0;
}

/* AH=71h AL=4Fh LFN FindNext — BX = handle (ignored, single find),
 * ES:DI = result buffer, SI = date format. */
static int dos_lfn_find_next(dos_proc_t *dos, dos_regs_t *regs) {
  return dos_lfn_find_scan(dos, regs);
}

/* AH=71h AL=A1h LFN FindClose — BX = handle (ignored). */
static int dos_lfn_find_close(dos_proc_t *dos, dos_regs_t *regs) {
  (void)regs;
  if (dos->find_fd >= 0) {
    sys_close(dos->find_fd);
    dos->find_fd = -1;
  }
  return 0;
}

/* AH=37h (undocumented): Get/Set switchar / device availability.
 *   AL=00h  Get switch char   → DL='/', AL=00
 *   AL=01h  Set switch char   (ignored, return AL=00)
 *   AL=02h  Get avail flag    → DL=00, AL=00
 *   AL=03h  Set avail flag    (ignored, return AL=00)
 *   others  AL=FFh (invalid)
 * COMMAND.COM and several utilities poke this to discover the `/`
 * option prefix.  We always report '/'. */
static int dos_switchar(dos_proc_t *dos, dos_regs_t *regs) {
  (void)dos;
  uint8_t al = (uint8_t)(regs->ax & 0xFFu);
  switch (al) {
    case 0x00:
      regs->dx = (uint16_t)((regs->dx & 0xFF00u) | '/');
      regs->ax = regs->ax & 0xFF00u;
      return 0;
    case 0x01:
    case 0x03:
      regs->ax = regs->ax & 0xFF00u;
      return 0;
    case 0x02:
      regs->dx = regs->dx & 0xFF00u;
      regs->ax = regs->ax & 0xFF00u;
      return 0;
    default:
      regs->ax = (uint16_t)((regs->ax & 0xFF00u) | 0xFFu);
      return 0;
  }
}

static int dos_terminate(dos_proc_t *dos, dos_regs_t *regs) {
  sys_exit(regs->ax & 0xFF);
  return 0; /* Not reached */
}

/* AH=43h Get/Set File Attributes.
 *   AL=00h  Get: returns CX = DOS attribute bits for the file at DS:DX.
 *   AL=01h  Set: CX = attribute bits to apply.  PPAP's VFS does not
 *           store read-only / hidden / system / archive, so set is a
 *           silent no-op after the file-existence check; the caller
 *           still sees CF=0 + CX reflecting the (unchanged) synthesised
 *           attributes, matching what a readback-verifying app
 *           expects.
 * AL values other than 0 or 1 return DOS_ERR_INVALID_FUNCTION. */
static int dos_get_set_attr(dos_proc_t *dos, dos_regs_t *regs) {
  uint8_t sub = regs->ax & 0xFF;
  if (sub != 0x00 && sub != 0x01) return -DOS_ERR_INVALID_FUNCTION;

  int rc = dos_resolve_user_path(dos, regs->ds, regs->dx);
  if (rc < 0) return rc;

  /* Pull the resolved ASCIIZ path out of the kernel scratch slot so it
   * can be handed to mod_vfs.lookup, which wants a C string pointer. */
  char path[DOS_PATH_SCRATCH_MAX];
  for (uint16_t i = 0; i < DOS_PATH_SCRATCH_MAX; i++) {
    uint8_t b;
    mem_region_page_read(dos_data_page, (uint16_t)(DOS_PATH_SCRATCH_OFF + i),
                         &b, 1);
    path[i] = (char)b;
    if (!b) break;
  }
  path[DOS_PATH_SCRATCH_MAX - 1] = '\0';

  vnode_t *vn = NULL;
  int err = mod_vfs.lookup(path, &vn);
  if (err) return -dos_errno_to_dos(err);

  struct stat st;
  err = mod_vfs.vnode_stat(vn, &st);
  mod_vfs.vnode_release(vn);
  if (err) return -dos_errno_to_dos(err);

  /* VFS does not track DOS attribute bits; synthesise the minimum:
   * DIRECTORY (0x10) for directories, ARCHIVE (0x20) for everything
   * else.  Both GET and SET return the same synthesised value. */
  regs->cx = S_ISDIR(st.st_mode) ? 0x0010u : 0x0020u;
  return 0;
}

/* AH=44h IOCTL sub-function dispatch.
 *
 * AL=00h  Get Device Info — DX = device-info word for BX = handle.
 *         Handles 0..4 (console / serial) report as CON (0x80D3):
 *           bit 15 = 1 : character device
 *           bit  7 = 1 : character device (legacy-redundant)
 *           bit  6 = 1 : "no EOF on input" (console semantics)
 *           bit  4 = 1 : special device
 *           bit  1 = 1 : stdout
 *           bit  0 = 1 : stdin
 *         File handles report 0x0000 (disk file on current drive).
 *
 * AL=01h  Set Device Info — accepted for open handles.  PPAP's handle
 *         layer has no backing storage for DOS device-info bits
 *         (binary mode, raw, etc.), so we discard them silently but
 *         still succeed so apps that set+readback keep working.
 *
 * AL=06h  Get Input Status   — AL=0xFF for every open handle.
 * AL=07h  Get Output Status  — AL=0xFF for every open handle.
 *         Real DOS distinguishes console-ready vs file-at-EOF, but
 *         for PPAP the subsequent READ/WRITE delivers exactly what
 *         the backing fd produces, so a blanket "ready" is truthful.
 *
 * AL=08h  Check If Block Device Removable — AX=1 (fixed) for every
 *         drive: PPAP's UFS storage is not ejectable.
 * AL=09h  Check If Block Device Remote    — DX=0 (local) for every
 *         drive.
 * AL=0Bh  Set Sharing Retry Count         — accept and discard.
 *         PPAP's VFS does not implement SHARE-style retries.
 *
 * Other AL values fall through to DOS_ERR_INVALID_FUNCTION. */
static int dos_ioctl(dos_proc_t *dos, dos_regs_t *regs) {
  uint8_t al = (uint8_t)(regs->ax & 0xFFu);

  switch (al) {
    case 0x00: {
      uint16_t handle = regs->bx;
      if (handle >= DOS_MAX_HANDLES || dos->handle_to_fd[handle] < 0)
        return -DOS_ERR_INVALID_HANDLE;
      uint16_t info = (handle <= 4) ? 0x80D3u : 0x0000u;
      regs->dx = info;
      regs->ax = info;
      return 0;
    }

    case 0x01: {
      uint16_t handle = regs->bx;
      if (handle >= DOS_MAX_HANDLES || dos->handle_to_fd[handle] < 0)
        return -DOS_ERR_INVALID_HANDLE;
      return 0;
    }

    case 0x06:
    case 0x07: {
      uint16_t handle = regs->bx;
      if (handle >= DOS_MAX_HANDLES || dos->handle_to_fd[handle] < 0)
        return -DOS_ERR_INVALID_HANDLE;
      regs->ax = 0x00FFu;
      return 0;
    }

    case 0x08:
      regs->ax = 0x0001u;
      return 0;

    case 0x09:
      regs->dx = 0x0000u;
      return 0;

    case 0x0B:
      return 0;
  }

  return -DOS_ERR_INVALID_FUNCTION;
}

/* Vectors the kernel uses for its own ISRs or panic stubs.  Writes from
 * the guest are silently ignored so a misbehaving DOS program can't
 * wedge the timer, DOS dispatcher, PPAP syscall, or CPU-fault panic
 * path.  Reads still return whatever is in the real IVT. */
static int dos_vec_is_protected(uint8_t vec) {
  if (vec <= 0x08) return 1;                /* CPU exceptions + timer */
  if (vec == 0x18 || vec == 0x19) return 1; /* ROM BASIC / bootstrap */
  if (vec == 0x20 || vec == 0x21) return 1; /* DOS entry points */
  if (vec == 0x30) return 1;                /* PPAP syscall */
  return 0;
}

/* AH=35h Get Interrupt Vector.
 *   AL = vector number.  Returns ES:BX = IP:CS from the IVT at 0:AL*4.
 *
 * Reads through cpu_ops so the native ia16 path hits the real IVT and
 * the eCPU path (future) hits the emulated memory.  No CF change. */
static int dos_get_int_vector(dos_proc_t *dos, dos_regs_t *regs) {
  uint8_t vec = (uint8_t)(regs->ax & 0xFF);
  uint32_t addr = (uint32_t)vec * 4;
  regs->bx = current->cpu_ops->read16(dos->cpu_state, addr);
  regs->es = current->cpu_ops->read16(dos->cpu_state, addr + 2);
  return 0;
}

/* AH=25h Set Interrupt Vector.
 *   AL = vector number, DS:DX = new IP:CS.
 *
 * First touch of a non-protected vector snapshots the previous IP:CS
 * into the cold slot so msdos_on_exit can restore it.  Further writes
 * to the same vector skip the snapshot.  When the per-process save
 * table is full, additional new-vector writes still go through but
 * won't be restored — we warn once.  Writes to protected vectors are
 * dropped.
 *
 * noinline: this handler allocates dos_proc_cold_t on its frame, and
 * without the attribute it gets inlined into dos_int21h_dispatch's
 * giant switch — accumulating ~60 B into the dispatch frame that's
 * live on every INT 21h, not just AH=25h.  Keep the cold-path cost
 * off the hot-path stack. */
__attribute__((noinline)) static int dos_set_int_vector(dos_proc_t *dos,
                                                        dos_regs_t *regs) {
  uint8_t vec = (uint8_t)(regs->ax & 0xFF);
  if (dos_vec_is_protected(vec)) {
    mod_vfs.klogf("[msdos] AH=25h vec=%x protected (dropped)\n", (unsigned)vec);
    return 0;
  }

  dos_proc_cold_t cold;
  dos_get_cold(current, &cold);
  int seen = 0;
  for (int i = 0; i < cold.ivt_saved_count; i++) {
    if (cold.ivt_saved_vec[i] == vec) {
      seen = 1;
      break;
    }
  }
  uint32_t addr = (uint32_t)vec * 4;
  if (!seen) {
    if (cold.ivt_saved_count < DOS_IVT_SAVE_MAX) {
      uint8_t slot = cold.ivt_saved_count;
      cold.ivt_saved_vec[slot] = vec;
      cold.ivt_saved_ip[slot] = current->cpu_ops->read16(dos->cpu_state, addr);
      cold.ivt_saved_cs[slot] =
          current->cpu_ops->read16(dos->cpu_state, addr + 2);
      cold.ivt_saved_count = (uint8_t)(slot + 1);
      dos_put_cold(current, &cold);
    } else {
      mod_vfs.klogf("[msdos] AH=25h vec=%x save table full (no restore)\n",
                    (unsigned)vec);
    }
  }
  current->cpu_ops->write16(dos->cpu_state, addr, regs->dx);
  current->cpu_ops->write16(dos->cpu_state, addr + 2, regs->ds);
  return 0;
}

static int dos_open_common(dos_proc_t *dos, dos_regs_t *regs, int flags) {
  int rc = dos_resolve_user_path(dos, regs->ds, regs->dx);
  if (rc < 0) return rc;

  long fd = sys_open(dos_data_page, DOS_PATH_SCRATCH_OFF, flags, 0644);
  if (fd < 0) return -dos_errno_to_dos((int)fd);

  int h = dos_alloc_handle(dos, (int)fd);
  if (h < 0) {
    sys_close(fd);
    return h;
  }
  regs->ax = (uint16_t)h;
  return 0;
}

static int dos_create(dos_proc_t *dos, dos_regs_t *regs) {
  /* CX = attribute; only RO bit honored, others ignored for now. */
  return dos_open_common(dos, regs, O_WRONLY | O_CREAT | O_TRUNC);
}

static int dos_open_file(dos_proc_t *dos, dos_regs_t *regs) {
  int access = regs->ax & 0x07;
  int flags;
  switch (access) {
    case 0:
      flags = O_RDONLY;
      break;
    case 1:
      flags = O_WRONLY;
      break;
    case 2:
      flags = O_RDWR;
      break;
    default:
      return -DOS_ERR_INVALID_ACCESS;
  }
  return dos_open_common(dos, regs, flags);
}

static int dos_close_file(dos_proc_t *dos, dos_regs_t *regs) {
  int handle = regs->bx;
  int fd = dos_lookup_fd(dos, handle);
  if (fd < 0) return fd;

  /* Refuse to close reserved standard handles — DOS permits it but it
   * breaks subsequent INT 21h I/O.  Report success without touching fd. */
  if (handle < DOS_FIRST_USER_HANDLE) return 0;

  long rc = sys_close(fd);
  dos->handle_to_fd[handle] = -1;
  if (rc < 0) return -dos_errno_to_dos((int)rc);
  return 0;
}

/* AH=3Fh READ / AH=40h WRITE — BX=handle, CX=count, DS:DX=buffer. */

static int dos_rw_common(dos_proc_t *dos, dos_regs_t *regs, int is_write) {
  int handle = regs->bx;
  int fd = dos_lookup_fd(dos, handle);
  if (fd < 0) return fd;

  uint16_t count = regs->cx;
  if (count == 0) {
    regs->ax = 0;
    return 0;
  }

  /* DS:DX is a far real-mode pointer.  Compute its 20-bit flat address,
   * then express it as (page, off) within the proc's contiguous image.
   * The proc owns image.data.{base_page, size} as one allocation block;
   * any access in [base_linear, base_linear+size) is in-range.  This
   * replaces the old proc_user_ptr_to_page_ref call, which assumed the
   * caller's offset is relative to the proc's implicit data segment —
   * wrong whenever the app picks its own DS (e.g. zork1 holds file
   * buffers in a separate paragraph of its conventional-memory window). */
  user_page_ref_t ref;
  page_id_t base = current->image.data.base_page;
  uint32_t base_linear = mem_region_page_linear(base);
  uint32_t flat = ((uint32_t)regs->ds << 4) + regs->dx;
  if (flat < base_linear ||
      flat + (uint32_t)count > base_linear + current->image.data.size)
    return -DOS_ERR_INVALID_ACCESS;
  uint32_t off_in_proc = flat - base_linear;
  ref.page = base + (page_id_t)(off_in_proc / PAGE_SIZE);
  ref.off = (uint16_t)(off_in_proc % PAGE_SIZE);

  long n = is_write ? sys_write(fd, ref.page, ref.off, count)
                    : sys_read(fd, ref.page, ref.off, count);
  if (n < 0) return -dos_errno_to_dos((int)n);
  regs->ax = (uint16_t)n;
  return 0;
}

static int dos_read(dos_proc_t *dos, dos_regs_t *regs) {
  return dos_rw_common(dos, regs, 0);
}

static int dos_write(dos_proc_t *dos, dos_regs_t *regs) {
  return dos_rw_common(dos, regs, 1);
}

/* AH=41h DELETE — DS:DX = path. */
static int dos_delete(dos_proc_t *dos, dos_regs_t *regs) {
  int rc = dos_resolve_user_path(dos, regs->ds, regs->dx);
  if (rc < 0) return rc;

  long r = sys_unlink(dos_data_page, DOS_PATH_SCRATCH_OFF);
  if (r < 0) return -dos_errno_to_dos((int)r);
  return 0;
}

/* AH=42h LSEEK — BX=handle, AL=whence (0/1/2 → SET/CUR/END),
 * CX:DX = signed 32-bit offset (CX high, DX low).  Returns DX:AX as
 * the new absolute position. */
static int dos_lseek(dos_proc_t *dos, dos_regs_t *regs) {
  int handle = regs->bx;
  int fd = dos_lookup_fd(dos, handle);
  if (fd < 0) return fd;

  int whence = regs->ax & 0x07;
  if (whence > 2) return -DOS_ERR_INVALID_FUNCTION;

  int32_t off = (int32_t)(((uint32_t)regs->cx << 16) | regs->dx);
  long pos = sys_lseek(fd, off, whence);
  if (pos < 0) return -dos_errno_to_dos((int)pos);

  regs->ax = (uint16_t)(pos & 0xFFFF);
  regs->dx = (uint16_t)((uint32_t)pos >> 16);
  return 0;
}

/* AH=39h MKDIR / AH=3Ah RMDIR — DS:DX = path. */
static int dos_mkdir(dos_proc_t *dos, dos_regs_t *regs) {
  int rc = dos_resolve_user_path(dos, regs->ds, regs->dx);
  if (rc < 0) return rc;
  long r = sys_mkdir(dos_data_page, DOS_PATH_SCRATCH_OFF, 0755);
  if (r < 0) return -dos_errno_to_dos((int)r);
  return 0;
}

static int dos_rmdir(dos_proc_t *dos, dos_regs_t *regs) {
  int rc = dos_resolve_user_path(dos, regs->ds, regs->dx);
  if (rc < 0) return rc;
  long r = sys_rmdir(dos_data_page, DOS_PATH_SCRATCH_OFF);
  if (r < 0) return -dos_errno_to_dos((int)r);
  return 0;
}

/* AH=3Bh CHDIR — DS:DX = path.  DOS chdir is per-DOS-process state
 * (per drive cwd_c / cwd_z), independent of the kernel's process cwd.
 * Resolution rules from §4.4: drive letter selects cwd_c or cwd_z;
 * absolute path replaces it, relative path appends. */
static int dos_chdir(dos_proc_t *dos, dos_regs_t *regs) {
  /* Read DOS path from user segment into a small kstack buffer.  The
   * subsequent path-existence check happens via dos_resolve_user_path
   * + a sys_open + sys_close round-trip; the resolved scratch path
   * then mirrors what file ops will see, so chdir leaves the bridge
   * in a self-consistent state. */
  int rc = dos_resolve_user_path(dos, regs->ds, regs->dx);
  if (rc < 0) return rc;

  /* Verify the target exists and is a directory by opening it
   * read-only.  If sys_open succeeds we close immediately. */
  long fd = sys_open(dos_data_page, DOS_PATH_SCRATCH_OFF, O_RDONLY, 0);
  if (fd < 0) return -dos_errno_to_dos((int)fd);
  sys_close(fd);

  /* Re-parse the DOS path to decide which drive's cwd to update.
   * Read raw DOS path again into a kstack buffer (small, <= 64). */
  uint8_t drive = dos->current_drive;
  uint16_t in_off = regs->dx;
  uint8_t c0 = (uint8_t)current->cpu_ops->read8(
      dos->cpu_state, ((uint32_t)regs->ds << 4) + in_off);
  if (c0 && c0 != '\\' && c0 != '/') {
    uint8_t c1 = (uint8_t)current->cpu_ops->read8(
        dos->cpu_state, ((uint32_t)regs->ds << 4) + in_off + 1);
    if (c1 == ':') {
      char d = (char)c0;
      if (d >= 'a' && d <= 'z') d = (char)(d - ('a' - 'A'));
      if (d == 'C')
        drive = 2;
      else if (d == 'Z')
        drive = 25;
      else
        return -DOS_ERR_INVALID_DRIVE;
      in_off += 2;
    }
  }

  uint16_t cwd_off;
  if (drive == 2)
    cwd_off = (uint16_t)(dos_paths_base(current) + DOS_CWD_C_FIELD_OFF);
  else if (drive == 25)
    cwd_off = (uint16_t)(dos_paths_base(current) + DOS_CWD_Z_FIELD_OFF);
  else
    return -DOS_ERR_INVALID_DRIVE;

  /* Copy the remaining DOS path (post-drive-letter) into the cwd slot
   * on dos_data_page, converting backslashes and skipping a leading
   * separator. */
  uint8_t peek = (uint8_t)current->cpu_ops->read8(
      dos->cpu_state, ((uint32_t)regs->ds << 4) + in_off);
  if (peek == '/' || peek == '\\') in_off++;

  int n = 0;
  for (;;) {
    if (n >= DOS_PATH_MAX - 1) return -DOS_ERR_PATH_NOT_FOUND;
    uint8_t b = (uint8_t)current->cpu_ops->read8(
        dos->cpu_state, ((uint32_t)regs->ds << 4) + in_off++);
    if (b == 0) break;
    dos_scratch_putb((uint16_t)(cwd_off + n), (b == '\\') ? '/' : b);
    n++;
  }
  dos_scratch_putb((uint16_t)(cwd_off + n), 0);

  /* Switch active drive too (DOS does this implicitly). */
  dos->current_drive = drive;
  return 0;
}

/* AH=47h GETCWD — DL = drive (1=A, 2=B, ..., 0=current),
 * DS:SI = buffer (≥ 64 bytes per DOS spec; we cap by DOS_PATH_MAX).
 * Writes ASCIIZ path *without* drive letter or leading slash. */
static int dos_getcwd(dos_proc_t *dos, dos_regs_t *regs) {
  uint8_t dl = (uint8_t)(regs->dx & 0xFF);
  uint8_t drive = (dl == 0) ? dos->current_drive : (uint8_t)(dl - 1);
  uint16_t cwd_off;
  if (drive == 2)
    cwd_off = (uint16_t)(dos_paths_base(current) + DOS_CWD_C_FIELD_OFF);
  else if (drive == 25)
    cwd_off = (uint16_t)(dos_paths_base(current) + DOS_CWD_Z_FIELD_OFF);
  else
    return -DOS_ERR_INVALID_DRIVE;

  /* Skip any leading '/' so the result is root-relative per DOS. */
  uint16_t idx = 0;
  while (idx < DOS_PATH_MAX &&
         dos_scratch_getb((uint16_t)(cwd_off + idx)) == '/') {
    idx++;
  }

  /* Write ASCIIZ to user DS:SI byte-by-byte (fits in 64 bytes). */
  uint16_t out = regs->si;
  int n = 0;
  for (;; n++) {
    if (n >= DOS_PATH_MAX) return -DOS_ERR_PATH_NOT_FOUND;
    uint8_t b = dos_scratch_getb((uint16_t)(cwd_off + idx + n));
    current->cpu_ops->write8(dos->cpu_state,
                             ((uint32_t)regs->ds << 4) + out + n, b);
    if (b == 0) break;
  }
  /* DOS spec: AX is preserved on success.  Some sources say AX=0100h
   * — we leave it untouched to match common reference docs. */
  return 0;
}

/* AH=45h DUP — BX = handle.  Returns AX = new handle. */
static int dos_dup(dos_proc_t *dos, dos_regs_t *regs) {
  int handle = regs->bx;
  int fd = dos_lookup_fd(dos, handle);
  if (fd < 0) return fd;

  long newfd = sys_dup(fd);
  if (newfd < 0) return -dos_errno_to_dos((int)newfd);

  int h = dos_alloc_handle(dos, (int)newfd);
  if (h < 0) {
    sys_close(newfd);
    return h;
  }
  regs->ax = (uint16_t)h;
  return 0;
}

/* AH=46h DUP2 — BX = source handle, CX = target handle. */
static int dos_dup2(dos_proc_t *dos, dos_regs_t *regs) {
  int src_h = regs->bx;
  int dst_h = regs->cx;
  int src_fd = dos_lookup_fd(dos, src_h);
  if (src_fd < 0) return src_fd;
  if (dst_h < 0 || dst_h >= DOS_MAX_HANDLES) return -DOS_ERR_INVALID_HANDLE;

  /* If destination DOS handle already maps to an fd, close the
   * underlying fd first (DOS DUP2 closes the destination if open). */
  int dst_fd_existing = dos->handle_to_fd[dst_h];
  if (dst_fd_existing >= 0 && dst_h >= DOS_FIRST_USER_HANDLE)
    sys_close(dst_fd_existing);

  long newfd = sys_dup(src_fd);
  if (newfd < 0) return -dos_errno_to_dos((int)newfd);
  dos->handle_to_fd[dst_h] = (int)newfd;
  return 0;
}

/* AH=56h RENAME — DS:DX = old path, ES:DI = new path. */
static int dos_rename(dos_proc_t *dos, dos_regs_t *regs) {
  /* Resolve old path into the primary scratch slot, then copy the
   * resolved bytes into the secondary slot so the next resolve
   * doesn't clobber them. */
  int rc = dos_resolve_user_path(dos, regs->ds, regs->dx);
  if (rc < 0) return rc;
  uint8_t buf;
  uint16_t i = 0;
  for (; i < DOS_PATH_SCRATCH_MAX; i++) {
    mem_region_page_read(dos_data_page, (uint16_t)(DOS_PATH_SCRATCH_OFF + i),
                         &buf, 1);
    mem_region_page_write(dos_data_page, (uint16_t)(DOS_PATH_SCRATCH2_OFF + i),
                          &buf, 1);
    if (buf == 0) break;
  }
  if (i >= DOS_PATH_SCRATCH_MAX) return -DOS_ERR_PATH_NOT_FOUND;

  /* Resolve new path into the primary slot. */
  rc = dos_resolve_user_path(dos, regs->es, regs->di);
  if (rc < 0) return rc;

  long r = sys_rename(dos_data_page, DOS_PATH_SCRATCH2_OFF, dos_data_page,
                      DOS_PATH_SCRATCH_OFF);
  if (r < 0) return -dos_errno_to_dos((int)r);
  return 0;
}

/* ── MCB chain primitives ──────────────────────────────────────────── */

typedef struct dos_mcb_view {
  uint8_t sig;
  uint16_t owner;
  uint16_t size;
} dos_mcb_view_t;

static void dos_mcb_read(page_id_t base_page, uint32_t off,
                         dos_mcb_view_t *out) {
  uint8_t hdr[5];
  page_id_t pg = base_page + (page_id_t)(off / PAGE_SIZE);
  uint16_t pgo = (uint16_t)(off % PAGE_SIZE);
  mem_region_page_read(pg, pgo, hdr, 5);
  out->sig = hdr[DOS_MCB_OFF_SIG];
  out->owner = (uint16_t)hdr[DOS_MCB_OFF_OWNER] |
               ((uint16_t)hdr[DOS_MCB_OFF_OWNER + 1] << 8);
  out->size = (uint16_t)hdr[DOS_MCB_OFF_SIZE] |
              ((uint16_t)hdr[DOS_MCB_OFF_SIZE + 1] << 8);
}

static void dos_mcb_write(page_id_t base_page, uint32_t off, uint8_t sig,
                          uint16_t owner, uint16_t size) {
  uint8_t b[DOS_MCB_BYTES];
  __builtin_memset(b, 0, sizeof(b));
  b[DOS_MCB_OFF_SIG] = sig;
  b[DOS_MCB_OFF_OWNER] = (uint8_t)(owner & 0xFF);
  b[DOS_MCB_OFF_OWNER + 1] = (uint8_t)(owner >> 8);
  b[DOS_MCB_OFF_SIZE] = (uint8_t)(size & 0xFF);
  b[DOS_MCB_OFF_SIZE + 1] = (uint8_t)(size >> 8);
  page_id_t pg = base_page + (page_id_t)(off / PAGE_SIZE);
  uint16_t pgo = (uint16_t)(off % PAGE_SIZE);
  mem_region_page_write(pg, pgo, b, DOS_MCB_BYTES);
}

/* The PSP segment of the calling DOS process — also the owner stored
 * in every MCB the process allocates. */
static uint16_t dos_caller_psp(uint32_t base_linear) {
  return (uint16_t)((base_linear >> 4) + 1u);
}

/* Resolve a DOS block segment to an MCB run-offset; validates that the
 * MCB falls inside the proc image.  Returns 0 on success, negative DOS
 * error otherwise. */
static int dos_mcb_offset_for_seg(uint16_t seg, uint32_t base_linear,
                                  uint32_t run_size, uint32_t *out_off) {
  uint32_t mcb_flat = ((uint32_t)seg - 1u) << 4;
  if (mcb_flat < base_linear ||
      mcb_flat + DOS_MCB_BYTES > base_linear + run_size)
    return -DOS_ERR_INSUFFICIENT_MEMORY;
  *out_off = mcb_flat - base_linear;
  return 0;
}

/* AH=4Ah Resize Memory Block.
 *
 *   ES = block segment (caller PSP for the main block; ES-1 = MCB seg)
 *   BX = new size in paragraphs
 *
 * On success: CF=0.  On failure: CF=1, AX=8 (insufficient memory),
 * BX = max paragraphs available for this block.
 *
 * Handles both 'Z' (chain end) and 'M' (mid-chain) blocks.  For 'M',
 * the immediately-following block is consulted: if free, it can be
 * absorbed for growth or partially split off when shrinking. */
static int dos_resize_block(dos_proc_t *dos, dos_regs_t *regs) {
  (void)dos;

  uint16_t es = regs->es;
  uint16_t new_size = regs->bx;

  page_id_t base_page = current->image.data.base_page;
  uint32_t base_linear = mem_region_page_linear(base_page);
  uint32_t run_size = current->image.data.size;
  uint16_t caller_psp = dos_caller_psp(base_linear);

  uint32_t mcb_off;
  int rc = dos_mcb_offset_for_seg(es, base_linear, run_size, &mcb_off);
  if (rc < 0) {
    regs->bx = 0;
    return rc;
  }

  dos_mcb_view_t cur;
  dos_mcb_read(base_page, mcb_off, &cur);
  if ((cur.sig != DOS_MCB_SIG_M && cur.sig != DOS_MCB_SIG_Z) ||
      cur.owner != caller_psp) {
    regs->bx = 0;
    return -DOS_ERR_INSUFFICIENT_MEMORY;
  }

  /* Compute max_size and the sig that should follow our resized block. */
  uint16_t max_size;
  uint8_t after_sig;
  int absorb_next = 0;
  if (cur.sig == DOS_MCB_SIG_Z) {
    /* Last in chain — payload always extends to run end, so cur.size
     * is already the max. */
    max_size = cur.size;
    after_sig = DOS_MCB_SIG_Z;
  } else {
    uint32_t next_off = mcb_off + DOS_MCB_BYTES + (uint32_t)cur.size * 16u;
    if (next_off >= run_size) {
      /* Corrupt: 'M' but no next.  Fail safely. */
      regs->bx = cur.size;
      return -DOS_ERR_INSUFFICIENT_MEMORY;
    }
    dos_mcb_view_t next;
    dos_mcb_read(base_page, next_off, &next);
    if ((next.sig == DOS_MCB_SIG_M || next.sig == DOS_MCB_SIG_Z) &&
        next.owner == 0) {
      max_size = (uint16_t)(cur.size + 1u + next.size);
      after_sig = next.sig;
      absorb_next = 1;
    } else {
      max_size = cur.size;
      /* Next is owned and stays in the chain; the new free block (if any)
       * we insert below sits between us and that owned next, so it must
       * be 'M' (more-follows). */
      after_sig = DOS_MCB_SIG_M;
    }
  }

  if (new_size > max_size) {
    regs->bx = max_size;
    return -DOS_ERR_INSUFFICIENT_MEMORY;
  }
  if (new_size == cur.size && !absorb_next) return 0;

  uint16_t free_remainder = (uint16_t)(max_size - new_size);
  uint8_t cur_new_sig;
  if (free_remainder == 0) {
    /* Consume everything available; current takes whatever after_sig
     * said the structure past max would look like. */
    cur_new_sig = after_sig;
  } else {
    /* Split off a new free block after the new payload. */
    cur_new_sig = DOS_MCB_SIG_M;
    uint32_t new_free_off = mcb_off + DOS_MCB_BYTES + (uint32_t)new_size * 16u;
    uint16_t new_free_size = (uint16_t)(free_remainder - 1u);
    dos_mcb_write(base_page, new_free_off, after_sig, 0, new_free_size);
  }

  dos_mcb_write(base_page, mcb_off, cur_new_sig, caller_psp, new_size);
  return 0;
}

/* AH=48h Allocate Memory Block.
 *
 *   BX = paragraphs requested
 *
 * On success: AX = segment of allocated block, CF=0.
 * On failure: AX=8, BX=largest free block paragraphs, CF=1.
 *
 * First-fit walk over the in-run MCB chain.  When a free block large
 * enough is found, it is split into [allocated][free remainder]; on
 * exact fit, the existing free block is simply marked owned. */
static int dos_alloc_block(dos_proc_t *dos, dos_regs_t *regs) {
  (void)dos;
  uint16_t want = regs->bx;

  page_id_t base_page = current->image.data.base_page;
  uint32_t base_linear = mem_region_page_linear(base_page);
  uint32_t run_size = current->image.data.size;
  uint16_t caller_psp = dos_caller_psp(base_linear);

  uint32_t off = 0;
  uint16_t largest = 0;
  uint32_t found_off = 0xFFFFFFFFu;
  uint8_t found_sig = 0;
  uint16_t found_size = 0;

  while (off < run_size) {
    dos_mcb_view_t m;
    dos_mcb_read(base_page, off, &m);
    if (m.sig != DOS_MCB_SIG_M && m.sig != DOS_MCB_SIG_Z) {
      /* Corrupt chain. */
      regs->bx = largest;
      return -DOS_ERR_INSUFFICIENT_MEMORY;
    }
    if (m.owner == 0) {
      if (m.size > largest) largest = m.size;
      if (found_off == 0xFFFFFFFFu && m.size >= want) {
        found_off = off;
        found_sig = m.sig;
        found_size = m.size;
      }
    }
    if (m.sig == DOS_MCB_SIG_Z) break;
    off += DOS_MCB_BYTES + (uint32_t)m.size * 16u;
  }

  if (found_off == 0xFFFFFFFFu) {
    regs->bx = largest;
    return -DOS_ERR_INSUFFICIENT_MEMORY;
  }

  if (want == found_size) {
    /* Exact fit — claim the existing block; sig unchanged. */
    dos_mcb_write(base_page, found_off, found_sig, caller_psp, want);
  } else {
    /* Split: [alloc 'M'][remaining free, sig=found_sig]. */
    uint32_t new_free_off = found_off + DOS_MCB_BYTES + (uint32_t)want * 16u;
    uint16_t new_free_size = (uint16_t)(found_size - want - 1u);
    dos_mcb_write(base_page, found_off, DOS_MCB_SIG_M, caller_psp, want);
    dos_mcb_write(base_page, new_free_off, found_sig, 0, new_free_size);
  }

  uint32_t alloc_flat = base_linear + found_off + DOS_MCB_BYTES;
  regs->ax = (uint16_t)(alloc_flat >> 4);
  return 0;
}

/* AH=49h Free Memory Block.
 *
 *   ES = block segment to free
 *
 * On success: CF=0.  On failure: CF=1, AX = DOS error.
 *
 * Marks the block free (owner=0) and forward-coalesces with the
 * immediately-following block if it is also free.  Backward coalesce
 * (merging into a preceding free block) is not done in D-5a.3 — it
 * would require a chain walk from the head; deferred until a workload
 * needs it. */
static int dos_free_block(dos_proc_t *dos, dos_regs_t *regs) {
  (void)dos;
  uint16_t es = regs->es;

  page_id_t base_page = current->image.data.base_page;
  uint32_t base_linear = mem_region_page_linear(base_page);
  uint32_t run_size = current->image.data.size;
  uint16_t caller_psp = dos_caller_psp(base_linear);

  uint32_t mcb_off;
  int rc = dos_mcb_offset_for_seg(es, base_linear, run_size, &mcb_off);
  if (rc < 0) return rc;

  dos_mcb_view_t cur;
  dos_mcb_read(base_page, mcb_off, &cur);
  if ((cur.sig != DOS_MCB_SIG_M && cur.sig != DOS_MCB_SIG_Z) ||
      cur.owner != caller_psp) {
    return -DOS_ERR_INSUFFICIENT_MEMORY;
  }

  uint8_t new_sig = cur.sig;
  uint16_t new_size = cur.size;

  /* Forward-coalesce with the immediately-following free block. */
  if (cur.sig == DOS_MCB_SIG_M) {
    uint32_t next_off = mcb_off + DOS_MCB_BYTES + (uint32_t)cur.size * 16u;
    if (next_off < run_size) {
      dos_mcb_view_t next;
      dos_mcb_read(base_page, next_off, &next);
      if ((next.sig == DOS_MCB_SIG_M || next.sig == DOS_MCB_SIG_Z) &&
          next.owner == 0) {
        new_size = (uint16_t)(new_size + 1u + next.size);
        new_sig = next.sig;
      }
    }
  }

  dos_mcb_write(base_page, mcb_off, new_sig, 0, new_size);
  return 0;
}

/* AH=52h Get DOS "List of Lists" (SYSVARS) pointer.
 *
 * Returns ES:BX pointing into a small fake SYSVARS region inside the
 * proc's PSP.  The only field MEM-style chain walkers consistently
 * read is the word at ES:[BX-2], which holds the segment of the first
 * MCB in the chain.  For PPAP that's (proc_seg - 1), the paragraph
 * holding the proc's main MCB header.
 *
 * The PSP "reserved" region at offset 0x3A..0x4F is unused by us, so
 * we stash:
 *   PSP[0x3E..0x3F]  first_mcb_seg = proc_seg - 1
 *   PSP[0x40+]       (the SYSVARS body MEM doesn't read from us)
 *
 * Real SYSVARS is a much richer structure (NUL device, drive table,
 * CDS, FCB tables, etc.).  We provide just enough for the chain-head
 * field to be valid; programs that read deeper fields will see PSP
 * bytes and may misbehave.  Acceptable for D-5a.4. */
static int dos_get_sysvars(dos_proc_t *dos, dos_regs_t *regs) {
  (void)dos;
  page_id_t base_page = current->image.data.base_page;
  uint32_t base_linear = mem_region_page_linear(base_page);
  uint16_t proc_seg = dos_caller_psp(base_linear);
  uint16_t first_mcb = (uint16_t)(proc_seg - 1u);

  /* PSP starts at base_id:DOS_MCB_BYTES.  Write the first-MCB segment
   * at PSP offset 0x3E (= base_id:0x4E). */
  uint8_t bytes[2] = {(uint8_t)(first_mcb & 0xFF), (uint8_t)(first_mcb >> 8)};
  mem_region_page_write(base_page, (uint16_t)(DOS_MCB_BYTES + 0x3Eu), bytes, 2);

  regs->es = proc_seg;
  regs->bx = 0x40;
  return 0;
}

/* AH=58h Get/Set Memory Allocation Strategy / UMB link state.
 *
 *   AL=00  Get strategy:    AX = strategy (0=first fit, etc.)
 *   AL=01  Set strategy:    BL = new strategy
 *   AL=02  Get UMB link:    AL = 0 (UMBs not in chain) or 1 (linked)
 *   AL=03  Set UMB link:    BL = 0/1
 *
 * PPAP has no UMBs and only first-fit allocation, so we report
 * AX=0 / AL=0 for the get sub-functions and ignore the set
 * sub-functions.  Writers (set sub-functions) succeed silently. */
static int dos_get_set_alloc(dos_proc_t *dos, dos_regs_t *regs) {
  (void)dos;
  uint8_t al = (uint8_t)(regs->ax & 0xFFu);
  switch (al) {
    case 0x00: /* Get strategy */
      regs->ax = 0;
      return 0;
    case 0x01: /* Set strategy */
      return 0;
    case 0x02: /* Get UMB link state */
      regs->ax = 0;
      return 0;
    case 0x03: /* Set UMB link state */
      return 0;
    default:
      return -DOS_ERR_INVALID_FUNCTION;
  }
}

/* AH=71h AL=43h LFN Extended Get/Set File Attributes.  Uses BL as the
 * sub-function selector (0=GET, 1=SET; values 2..0x0B for compressed
 * size / timestamps / creation time are beyond the SFN handler's scope
 * and fall through to INVALID_FUNCTION).  Otherwise identical to
 * AH=43h — DS:DX = path, CX = attribute word.  We route to the SFN
 * handler after copying BL into AL, then restore AX so the caller's
 * AH=71h AL=43h marker survives. */
static int dos_lfn_get_set_attr(dos_proc_t *dos, dos_regs_t *regs) {
  uint16_t saved_ax = regs->ax;
  regs->ax = (uint16_t)((saved_ax & 0xFF00u) | (regs->bx & 0xFFu));
  int ret = dos_get_set_attr(dos, regs);
  regs->ax = saved_ax;
  return ret;
}

/* AH=71h AL=6Ch LFN Extended Open/Create.
 *
 * Inputs:
 *   BX     = open mode.  Low 3 bits = access (0=R, 1=W, 2=RW).  Sharing
 *            (bits 4-6), inheritance (bit 7), critical-error (bit 13)
 *            and commit-on-write (bit 14) are accepted and discarded —
 *            PPAP's VFS does not implement DOS share modes.
 *   CX     = create attribute word — ignored (VFS does not store DOS
 *            attribute bits; see dos_get_set_attr).
 *   DX     = action code.  Low nibble (if file exists): 0=fail, 1=open,
 *            2=truncate.  High nibble (if file does not exist):
 *            0=fail, 1=create.
 *   DS:SI  = ASCIIZ filename.  NOTE: SI, not DX — distinct from
 *            AH=3Ch/3Dh.
 *   DI     = alias hint, ignored.
 *
 * Outputs (CF=0):
 *   AX = file handle, CX = action taken (1=opened existing,
 *   2=created new, 3=truncated existing).
 *
 * PPAP fcntl has no O_EXCL, so the "fail if exists" and "fail if not
 * exists" cases need a pre-lookup to enforce the constraint and to
 * compute the right CX action-taken value.  The lookup needs a C
 * string, so we copy the resolved path out of dos_data_page into a
 * 128-byte stack buffer (mirrors dos_get_set_attr).  noinline keeps
 * that buffer off the dispatcher's frame on every INT 21h. */
static int __attribute__((noinline)) dos_lfn_extended_open(dos_proc_t *dos,
                                                           dos_regs_t *regs) {
  int access = regs->bx & 0x07;
  int flags;
  switch (access) {
    case 0:
      flags = O_RDONLY;
      break;
    case 1:
      flags = O_WRONLY;
      break;
    case 2:
      flags = O_RDWR;
      break;
    default:
      return -DOS_ERR_INVALID_ACCESS;
  }

  uint8_t if_exists = (uint8_t)(regs->dx & 0x0F);
  uint8_t if_not_exists = (uint8_t)((regs->dx >> 4) & 0x0F);
  if (if_exists > 2 || if_not_exists > 1) return -DOS_ERR_INVALID_FUNCTION;
  if (if_exists == 0 && if_not_exists == 0) return -DOS_ERR_INVALID_FUNCTION;

  int rc = dos_resolve_user_path(dos, regs->ds, regs->si);
  if (rc < 0) return rc;

  char path[DOS_PATH_SCRATCH_MAX];
  for (uint16_t i = 0; i < DOS_PATH_SCRATCH_MAX; i++) {
    uint8_t b;
    mem_region_page_read(dos_data_page, (uint16_t)(DOS_PATH_SCRATCH_OFF + i),
                         &b, 1);
    path[i] = (char)b;
    if (!b) break;
  }
  path[DOS_PATH_SCRATCH_MAX - 1] = '\0';

  vnode_t *vn = NULL;
  int exists = (mod_vfs.lookup(path, &vn) == 0);
  if (vn) mod_vfs.vnode_release(vn);

  uint16_t action_taken;
  if (exists) {
    if (if_exists == 0) return -DOS_ERR_FILE_EXISTS;
    if (if_exists == 2) {
      flags |= O_TRUNC;
      action_taken = 3;
    } else {
      action_taken = 1;
    }
  } else {
    if (if_not_exists == 0) return -DOS_ERR_FILE_NOT_FOUND;
    flags |= O_CREAT;
    action_taken = 2;
  }

  long fd = sys_open(dos_data_page, DOS_PATH_SCRATCH_OFF, flags, 0644);
  if (fd < 0) return -dos_errno_to_dos((int)fd);

  int h = dos_alloc_handle(dos, (int)fd);
  if (h < 0) {
    sys_close(fd);
    return h;
  }
  regs->ax = (uint16_t)h;
  regs->cx = action_taken;
  return 0;
}

/* AH=71h sub-dispatch on AL.  Unknown sub-functions return
 * DOS_ERR_INVALID_FUNCTION — this is *not* the "LFN not installed"
 * reply (AX=0x7100 CF=1) which we deliberately avoid, since this
 * dispatcher implements several real LFN calls. */
static int dos_lfn_dispatch(dos_proc_t *dos, dos_regs_t *regs) {
  uint8_t al = (uint8_t)(regs->ax & 0xFFu);
  switch (al) {
    /* AL=39h/3Ah/3Bh/41h/47h/56h: thin LFN aliases for the SFN
     * handlers — identical register conventions (DS:DX path, DL drive
     * for AL=47h, ES:DI dest for rename) and the underlying VFS already
     * accepts long names. */
    case 0x39:
      return dos_mkdir(dos, regs);
    case 0x3A:
      return dos_rmdir(dos, regs);
    case 0x3B:
      return dos_chdir(dos, regs);
    case 0x41:
      return dos_delete(dos, regs);
    case 0x43:
      return dos_lfn_get_set_attr(dos, regs);
    case 0x47:
      return dos_getcwd(dos, regs);
    case 0x4E:
      return dos_lfn_find_first(dos, regs);
    case 0x4F:
      return dos_lfn_find_next(dos, regs);
    case 0x56:
      return dos_rename(dos, regs);
    case 0x60:
      return dos_lfn_truename(dos, regs);
    case 0x6C:
      return dos_lfn_extended_open(dos, regs);
    case 0xA1:
      return dos_lfn_find_close(dos, regs);
  }
  return -DOS_ERR_INVALID_FUNCTION;
}

int dos_int21h_dispatch(dos_proc_t *dos, dos_regs_t *regs) {
  uint8_t ah = regs->ax >> 8;
  int ret = 0;

  switch (ah) {
    case 0x01:
      ret = dos_read_char(dos, regs, 1);
      break;
    case 0x02:
      ret = dos_write_char(dos, regs);
      break;
    case 0x06:
      ret = dos_direct_console_io(dos, regs);
      break;
    case 0x07: /* Direct Console Input, no echo, no Ctrl-Break */
    case 0x08: /* Read char, no echo, Ctrl-Break checked (not enforced) */
      ret = dos_read_char(dos, regs, 0);
      break;
    case 0x09:
      ret = dos_print_string(dos, regs);
      break;
    case 0x0A:
      ret = dos_buffered_input(dos, regs);
      break;
    case 0x0B:
      ret = dos_check_input_status(dos, regs);
      break;
    case 0x19:
      ret = dos_get_current_drive(dos, regs);
      break;
    case 0x1A:
      ret = dos_set_dta(dos, regs);
      break;
    case 0x25:
      ret = dos_set_int_vector(dos, regs);
      break;
    case 0x2A:
      ret = dos_get_date(dos, regs);
      break;
    case 0x2C:
      ret = dos_get_time(dos, regs);
      break;
    case 0x2F:
      ret = dos_get_dta(dos, regs);
      break;
    case 0x30:
      ret = dos_get_version(dos, regs);
      break;
    case 0x35:
      ret = dos_get_int_vector(dos, regs);
      break;
    case 0x37:
      ret = dos_switchar(dos, regs);
      break;
    case 0x39:
      ret = dos_mkdir(dos, regs);
      break;
    case 0x3A:
      ret = dos_rmdir(dos, regs);
      break;
    case 0x3B:
      ret = dos_chdir(dos, regs);
      break;
    case 0x3C:
      ret = dos_create(dos, regs);
      break;
    case 0x3D:
      ret = dos_open_file(dos, regs);
      break;
    case 0x3E:
      ret = dos_close_file(dos, regs);
      break;
    case 0x3F:
      ret = dos_read(dos, regs);
      break;
    case 0x40:
      ret = dos_write(dos, regs);
      break;
    case 0x41:
      ret = dos_delete(dos, regs);
      break;
    case 0x42:
      ret = dos_lseek(dos, regs);
      break;
    case 0x43:
      ret = dos_get_set_attr(dos, regs);
      break;
    case 0x44:
      ret = dos_ioctl(dos, regs);
      break;
    case 0x45:
      ret = dos_dup(dos, regs);
      break;
    case 0x46:
      ret = dos_dup2(dos, regs);
      break;
    case 0x47:
      ret = dos_getcwd(dos, regs);
      break;
    case 0x48:
      ret = dos_alloc_block(dos, regs);
      break;
    case 0x49:
      ret = dos_free_block(dos, regs);
      break;
    case 0x4A:
      ret = dos_resize_block(dos, regs);
      break;
    case 0x4C:
      ret = dos_terminate(dos, regs);
      break;
    case 0x4E:
      ret = dos_find_first(dos, regs);
      break;
    case 0x4F:
      ret = dos_find_next(dos, regs);
      break;
    case 0x52:
      ret = dos_get_sysvars(dos, regs);
      break;
    case 0x56:
      ret = dos_rename(dos, regs);
      break;
    case 0x58:
      ret = dos_get_set_alloc(dos, regs);
      break;
    case 0x71:
      ret = dos_lfn_dispatch(dos, regs);
      break;
    default:
      mod_vfs.klogf("[msdos] unimpl INT 21h AH=%x AL=%x at CS:IP=%x:%x\n",
                    (unsigned)(regs->ax >> 8), (unsigned)(regs->ax & 0xFF),
                    (unsigned)regs->cs, (unsigned)regs->ip);
      ret = -DOS_ERR_INVALID_FUNCTION;
      break;
  }

  /* Handlers return a negated DOS error code (DOS_ERR_*) on failure;
   * dos_errno_to_dos has already done the errno→DOS translation by the
   * time we get here. */
  if (ret < 0) {
    regs->ax = (uint16_t)(-ret);
    regs->flags |= 0x0001;
  } else {
    regs->flags &= ~0x0001;
  }

  return ret;
}

#if defined(__ia16__)
int i16_dos_int21h_dispatch_native(dos_regs_t *regs) {
  dos_proc_t dos;
  dos_get_proc(current, &dos);
  int ret = dos_int21h_dispatch(&dos, regs);
  dos_put_proc(current, &dos);
  return ret;
}
#endif
