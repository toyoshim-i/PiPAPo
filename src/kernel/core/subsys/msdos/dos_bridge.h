/*
 * dos_bridge.h --- MS-DOS personality subsystem bridge
 */

#ifndef PPAP_KERNEL_CORE_SUBSYS_MSDOS_DOS_BRIDGE_H
#define PPAP_KERNEL_CORE_SUBSYS_MSDOS_DOS_BRIDGE_H

#include <stdint.h>

#include "kernel/core/cpu/ecpu_8086.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/subsys/subsys.h"

#define DOS_MAX_HANDLES 20
#define DOS_PATH_MAX 64

/* Per-process cap on hooked IVT vectors (AH=25h).  Real programs hook
 * well under a dozen; 4 keeps dos_proc_t small enough to fit PROC_MAX
 * copies under DOS_IO_SCRATCH_OFF. */
#define DOS_IVT_SAVE_MAX 4

/* DOS error codes (AX on error, returned with CF=1). */
#define DOS_ERR_INVALID_FUNCTION 1
#define DOS_ERR_FILE_NOT_FOUND 2
#define DOS_ERR_PATH_NOT_FOUND 3
#define DOS_ERR_TOO_MANY_OPEN 4
#define DOS_ERR_ACCESS_DENIED 5
#define DOS_ERR_INVALID_HANDLE 6
#define DOS_ERR_INSUFFICIENT_MEMORY 8
#define DOS_ERR_INVALID_ACCESS 12
#define DOS_ERR_INVALID_DRIVE 15
#define DOS_ERR_FILE_EXISTS 80

typedef struct dos_proc {
  /* Handle table: dos_handle -> ppap_fd (-1 = closed) */
  int handle_to_fd[DOS_MAX_HANDLES];

  uint8_t current_drive; /* 0=A, 1=B, 2=C (default), 25=Z */

  /* PSP location */
  uint16_t psp_seg;

  /* Memory access context */
  void *cpu_state;   /* CPU state for memory access */
  void *ecpu_memory; /* eCPU: flat memory pointer (NULL for native) */

  /* Path infrastructure (§4.4).  exec_dir is dirname() of the running
   * .COM/.EXE path, captured at load time; C: resolves against it.
   * cwd_c / cwd_z are relative directories within each drive, ""==root. */
  char exec_dir[DOS_PATH_MAX];
  char cwd_c[DOS_PATH_MAX];
  char cwd_z[DOS_PATH_MAX];

  /* Saved real-IVT entries for per-process restoration at exit.  On
   * first AH=25h write of each non-protected vector, the old IP:CS is
   * captured here; msdos_on_exit walks this list and writes them back. */
  uint8_t ivt_saved_vec[DOS_IVT_SAVE_MAX];
  uint16_t ivt_saved_ip[DOS_IVT_SAVE_MAX];
  uint16_t ivt_saved_cs[DOS_IVT_SAVE_MAX];
  uint8_t ivt_saved_count;
} dos_proc_t;

/* Layout matches the GP+IRET frame on the user stack at user_SP when
 * an INT 21h enters dos_trap.S — i.e. the same order as i16_syscall_isr
 * (INT 30h) pushes, so dos_trap.S can share the trap.S restore tail.
 *   offset  0 .. 16   pushed by dos_trap.S   (ES..AX, 9 words)
 *   offset 18 .. 22   pushed by CPU on INT   (IP, CS, FLAGS) */
typedef struct dos_regs {
  uint16_t es, ds, bp, di, si, dx, cx, bx, ax;
  uint16_t ip, cs, flags;
} dos_regs_t;

/* Global ops table for registration */
extern const subsys_ops_t msdos_subsys_ops;

/* Allocate per-process DOS state */
dos_proc_t *dos_proc_alloc(struct pcb *p);

/* Entry point from native/eCPU traps */
int dos_int21h_dispatch(dos_proc_t *dos, dos_regs_t *regs);

/* Captured by com_loader from argv[0] after on_init. */
void dos_set_exec_dir(struct pcb *p, const char *exec_path);

#endif /* PPAP_KERNEL_CORE_SUBSYS_MSDOS_DOS_BRIDGE_H */
