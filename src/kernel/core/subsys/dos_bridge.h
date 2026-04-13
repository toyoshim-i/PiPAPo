/*
 * dos_bridge.h --- MS-DOS personality subsystem bridge
 */

#ifndef PPAP_KERNEL_CORE_SUBSYS_DOS_BRIDGE_H
#define PPAP_KERNEL_CORE_SUBSYS_DOS_BRIDGE_H

#include <stdint.h>

#include "kernel/core/cpu/ecpu_8086.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/subsys/subsys.h"

#define DOS_MAX_HANDLES 20

typedef struct dos_proc {
  /* Handle table: dos_handle -> ppap_fd (-1 = closed) */
  int handle_to_fd[DOS_MAX_HANDLES];

  uint8_t current_drive; /* 0=A, 1=B, ... */

  /* PSP location */
  uint16_t psp_seg;

  /* Memory access context */
  void *cpu_state;   /* CPU state for memory access */
  void *ecpu_memory; /* eCPU: flat memory pointer (NULL for native) */
} dos_proc_t;

typedef struct dos_regs {
  uint16_t ax, bx, cx, dx, si, di, bp;
  uint16_t ds, es;
  uint16_t flags;
} dos_regs_t;

/* Global ops table for registration */
extern const subsys_ops_t msdos_subsys_ops;

/* Allocate per-process DOS state */
dos_proc_t *dos_proc_alloc(struct pcb *p);

/* Entry point from native/eCPU traps */
int dos_int21h_dispatch(dos_proc_t *dos, dos_regs_t *regs);

#endif /* PPAP_KERNEL_CORE_SUBSYS_DOS_BRIDGE_H */
