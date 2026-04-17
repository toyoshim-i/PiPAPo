/*
 * dos_bridge.c --- MS-DOS personality subsystem bridge
 */

#include "kernel/core/subsys/msdos/dos_bridge.h"

#include <stddef.h>

#include "kernel/common/core/proc_info.h"
#include "kernel/core/cpu/cpu.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/syscall/syscall.h"

/* Dedicated page for dos_proc_t array to avoid BSS overflow in core segment. */
static page_id_t dos_data_page = PAGE_ID_INVALID;

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

  dos_put_proc(p, &dos);
  p->subsys_data = (void *)(uintptr_t)(p - proc_table); /* Store slot index */
}

static void msdos_on_exit(struct pcb *p) { p->subsys_data = NULL; }

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
 * sys_read/sys_write resolve `user_ptr` through the per-arch user-pointer
 * translator.  On flat-memory architectures, that lookup happily finds the
 * page backing a kernel-stack address, so passing `&c` works.
 *
 * On ia16 the translator treats user_ptr strictly as a 16-bit offset within
 * the user's segment (see arch_user_ptr_to_page in src/arch/i16/.../arch.h),
 * so a kernel-stack pointer is misinterpreted as a user-segment offset and
 * the I/O lands on whatever happens to live at that user offset.
 *
 * For the few INT 21h handlers that move single bytes between fd 0/1 and
 * the kernel, stage the byte through a reserved slot in the .COM's PSP and
 * let sys_write/sys_read translate that proper user-segment offset.  PSP
 * bytes [0x60, 0x70) are unused by .COM convention. */
#if defined(__ia16__)
#define DOS_IO_SCRATCH_OFF 0x60u

static long dos_io_putc(uint8_t c) {
  page_id_t base = proc_page_backed_base(current);
  if (base == PAGE_ID_INVALID) return -1;
  mem_region_page_write(base, DOS_IO_SCRATCH_OFF, &c, 1);
  return sys_write(1, DOS_IO_SCRATCH_OFF, 1);
}

static long dos_io_getc(uint8_t *out) {
  page_id_t base = proc_page_backed_base(current);
  if (base == PAGE_ID_INVALID) return -1;
  long n = sys_read(0, DOS_IO_SCRATCH_OFF, 1);
  if (n != 1) return n;
  mem_region_page_read(base, DOS_IO_SCRATCH_OFF, out, 1);
  return 1;
}
#else
static long dos_io_putc(uint8_t c) { return sys_write(1, (uintptr_t)&c, 1); }

static long dos_io_getc(uint8_t *out) { return sys_read(0, (uintptr_t)out, 1); }
#endif

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
