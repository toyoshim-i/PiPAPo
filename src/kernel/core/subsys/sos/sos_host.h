/*
 * sos_host.h — S-OS subsystem memory image setup interface
 */

#ifndef PPAP_KERNEL_CORE_SUBSYS_SOS_SOS_HOST_H
#define PPAP_KERNEL_CORE_SUBSYS_SOS_SOS_HOST_H

#include <stdint.h>

#include "kernel/core/cpu/ecpu_z80.h"
#include "kernel/core/subsys/sos/sos_bridge.h"

/* Z80 address space: 64KB = 16 × 4KB pages */
#define SOS_Z80_MEM_PAGES 16

/* Per-process S-OS execution state (placed in the extra page after Z80 mem) */
typedef struct {
  z80_state_t z80;
  sos_state_t sos;
} sos_exec_state_t;

/*
 * sos_load_obj — parse _SOS header and build the emulated memory image.
 *
 * Validates header, sets up S-OS memory map (zero-page vectors, RST stubs,
 * monitor jump tables, work area), loads the payload at its specified load
 * address, and initializes Z80 CPU state (PC, SP, return address).  Also
 * sets the drive-A root from the executable path.
 *
 * The Z80 memory pointed to by cpu->memory must be pre-allocated (64 KB)
 * and the Z80 state must be initialized by the caller before this runs.
 *
 * Returns 0 on success, -ENOEXEC for malformed binaries.
 */
int sos_load_obj(z80_state_t *cpu, sos_state_t *sos, const uint8_t *file_buf,
                 uint32_t file_size, const char *path);

#endif /* PPAP_KERNEL_CORE_SUBSYS_SOS_SOS_HOST_H */
