/*
 * dos_bridge.c --- MS-DOS personality subsystem bridge
 */

#include "kernel/core/subsys/msdos/dos_bridge.h"

#include <stddef.h>

#include "common/errno.h"
#include "common/fcntl.h"
#include "kernel/common/core/proc_info.h"
#include "kernel/core/cpu/cpu.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/syscall/syscall.h"

/* Kernel-side scratch slots inside `dos_data_page` (allocated lazily in
 * msdos_on_init).  Used for staging paths and single-byte I/O without
 * borrowing user-segment memory.  Offsets sit comfortably above the
 * dos_proc_t array — see the static_assert below. */
#define DOS_IO_SCRATCH_OFF 0x800u   /* 1-byte staging for fd 0/1 byte I/O   */
#define DOS_PATH_SCRATCH_OFF 0x810u /* 128-byte staging for resolved paths */
#define DOS_PATH_SCRATCH_MAX 128u
#define DOS_SCRATCH_END (DOS_PATH_SCRATCH_OFF + DOS_PATH_SCRATCH_MAX)

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

static int msdos_on_crash(struct pcb *p, uint32_t *regs, uint16_t *exc,
                          int is_group0) {
  return 0;
}

static int msdos_on_signal(struct pcb *p, int sig, uint32_t *regs) { return 0; }

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

  dos_put_proc(p, &dos);
  p->subsys_data = (void *)(uintptr_t)(p - proc_table); /* Store slot index */
}

static void msdos_on_exit(struct pcb *p) { p->subsys_data = NULL; }

/* ── Exec-path capture ──────────────────────────────────────────────── */

void dos_set_exec_dir(struct pcb *p, const char *exec_path) {
  if (!p || !exec_path || dos_data_page == PAGE_ID_INVALID) return;

  dos_proc_t dos;
  dos_get_proc(p, &dos);

  const char *slash = NULL;
  for (const char *s = exec_path; *s; s++) {
    if (*s == '/') slash = s;
  }

  int n = 0;
  if (!slash || slash == exec_path) {
    dos.exec_dir[n++] = '/';
  } else {
    int len = (int)(slash - exec_path);
    if (len >= DOS_PATH_MAX) len = DOS_PATH_MAX - 1;
    for (int i = 0; i < len; i++) dos.exec_dir[n++] = exec_path[i];
  }
  dos.exec_dir[n] = '\0';

  dos_put_proc(p, &dos);
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
 * Assembly happens in a single kernel-stack buffer (sized to the
 * scratch slot, 128 B) and is flushed in one mem_region_page_write
 * call.  Compared to byte-at-a-time writes, this keeps code size
 * small at the cost of a modest kstack footprint. */

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

  const char *root;
  const char *cwd;
  if (drive == 2) {
    root = dos->exec_dir;
    cwd = dos->cwd_c;
  } else if (drive == 25) {
    root = "/";
    cwd = dos->cwd_z;
  } else {
    return -DOS_ERR_INVALID_DRIVE;
  }

  uint8_t peek = (uint8_t)current->cpu_ops->read8(
      dos->cpu_state, dos_to_linear(in_seg, in_off));
  int absolute = (peek == '/' || peek == '\\');
  if (absolute) in_off++;

  uint8_t buf[DOS_PATH_SCRATCH_MAX];
  uint16_t n = 0;

  /* Copy root, dropping a trailing '/' so we never emit '//'. */
  for (int i = 0; root[i]; i++) {
    if (root[i] == '/' && root[i + 1] == '\0') break;
    if (n >= DOS_PATH_SCRATCH_MAX - 1) return -DOS_ERR_PATH_NOT_FOUND;
    buf[n++] = (uint8_t)root[i];
  }

  if (!absolute && cwd[0]) {
    if (n >= DOS_PATH_SCRATCH_MAX - 1) return -DOS_ERR_PATH_NOT_FOUND;
    buf[n++] = '/';
    for (int i = 0; cwd[i]; i++) {
      if (n >= DOS_PATH_SCRATCH_MAX - 1) return -DOS_ERR_PATH_NOT_FOUND;
      buf[n++] = (uint8_t)cwd[i];
    }
  }

  peek = (uint8_t)current->cpu_ops->read8(dos->cpu_state,
                                          dos_to_linear(in_seg, in_off));
  if (peek) {
    if (n >= DOS_PATH_SCRATCH_MAX - 1) return -DOS_ERR_PATH_NOT_FOUND;
    buf[n++] = '/';
    for (;;) {
      uint8_t b = (uint8_t)current->cpu_ops->read8(
          dos->cpu_state, dos_to_linear(in_seg, in_off++));
      if (b == 0) break;
      if (b == '\\') b = '/';
      if (n >= DOS_PATH_SCRATCH_MAX - 1) return -DOS_ERR_PATH_NOT_FOUND;
      buf[n++] = b;
    }
  } else if (n == 0) {
    buf[n++] = '/';
  }
  buf[n] = '\0';

  mem_region_page_write(dos_data_page, DOS_PATH_SCRATCH_OFF, buf,
                        (uint16_t)(n + 1));
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
    case EACCES:
      return DOS_ERR_ACCESS_DENIED;
    case EBADF:
      return DOS_ERR_INVALID_HANDLE;
    case EEXIST:
      return DOS_ERR_FILE_EXISTS;
    case EISDIR:
      return DOS_ERR_ACCESS_DENIED;
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
  long n = sys_read(0, dos_data_page, DOS_IO_SCRATCH_OFF, 1);
  if (n != 1) return n;
  mem_region_page_read(dos_data_page, DOS_IO_SCRATCH_OFF, out, 1);
  return 1;
}

/* ── INT 21h functions ─────────────────────────────────────────────── */

static int dos_read_char(dos_proc_t *dos, dos_regs_t *regs, int echo) {
  uint8_t c;
  if (dos_io_getc(&c) == 1) {
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
    if (dos_io_getc(&c) != 1) break;
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

static int dos_check_input_status(dos_proc_t *dos, dos_regs_t *regs) {
  /* Use sys_poll or a simplified check for now */
  regs->ax = (regs->ax & 0xFF00) | 0xFF; /* Character available */
  /* TODO: implement actual poll */
  return 0;
}

static int dos_get_current_drive(dos_proc_t *dos, dos_regs_t *regs) {
  regs->ax = (regs->ax & 0xFF00) | dos->current_drive;
  return 0;
}

static int dos_get_date(dos_proc_t *dos, dos_regs_t *regs) {
  /* Simplified date: 2026-04-13 */
  regs->cx = 2026;
  regs->dx = 0x040D;                     /* DH=month, DL=day */
  regs->ax = (regs->ax & 0xFF00) | 0x01; /* Monday */
  return 0;
}

static int dos_get_time(dos_proc_t *dos, dos_regs_t *regs) {
  /* Simplified time: 12:00:00.00 */
  regs->cx = 0x0C00; /* CH=hour, CL=minute */
  regs->dx = 0x0000; /* DH=second, DL=1/100 sec */
  return 0;
}

static int dos_get_version(dos_proc_t *dos, dos_regs_t *regs) {
  regs->ax = 0x1E03; /* DOS 3.30 (AL=major, AH=minor) */
  regs->bx = 0x0000; /* Serial number */
  regs->cx = 0x0000;
  return 0;
}

static int dos_terminate(dos_proc_t *dos, dos_regs_t *regs) {
  sys_exit(regs->ax & 0xFF);
  return 0; /* Not reached */
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

  /* DS:DX is a user-segment offset; the translator already assumes the
   * caller's segment.  .COM programs keep DS=proc_seg by convention,
   * so passing regs->dx straight through is safe. */
  user_page_ref_t ref;
  if (proc_user_ptr_to_page_ref(current, regs->dx, &ref) < 0)
    return -DOS_ERR_INVALID_ACCESS;

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
    case 0x08:
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
    case 0x2A:
      ret = dos_get_date(dos, regs);
      break;
    case 0x2C:
      ret = dos_get_time(dos, regs);
      break;
    case 0x30:
      ret = dos_get_version(dos, regs);
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
    case 0x4C:
      ret = dos_terminate(dos, regs);
      break;
    default:
      /* Unknown function: set CF and return error 1 (invalid function) */
      regs->ax = 1;
      regs->flags |= 0x0001; /* Carry flag */
      return -1;
  }

  if (ret < 0) {
    regs->ax = (uint16_t)(-ret); /* Map errno to AX? Needs better mapping. */
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
