/*
 * exec_cpm.h — CP/M .COM binary loader
 *
 * Detects .COM files (by extension) and loads them into a Z80 emulator
 * instance for execution.  The CP/M BDOS/BIOS bridge handles system
 * calls via the eCPU trap mechanism.
 *
 * See docs/subsystems/cpm.md for the full design.
 */

#ifndef PPAP_EXEC_EXEC_CPM_H
#define PPAP_EXEC_EXEC_CPM_H

#include "kernel/proc/proc.h"
#include <stdint.h>

/*
 * cpm_detect — check if a file is a CP/M .COM binary.
 *
 * Detection is by filename extension (.COM or .com), since .COM files
 * have no magic number.  The file must also be small enough to fit in
 * the TPA (Transient Program Area, 0x0100–0xFE00).
 *
 * Returns 1 if .COM, 0 otherwise.
 */
int cpm_detect(const char *path, const uint8_t *file, uint32_t size);

/*
 * exec_cpm — load and set up a .COM binary for Z80 emulation.
 *
 * Allocates 64KB for the Z80 address space (16 pages), initialises the
 * CP/M memory map (zero page, BIOS stubs), loads the binary at 0x0100,
 * and sets up the Z80 emulator state with the BDOS trap handler.
 *
 * The process runs in kernel mode via ecpu_z80_ops.run(), which is
 * called from the CP/M subsystem's on_init hook.
 *
 * Returns 0 on success, negative errno on failure.
 */
int exec_cpm(pcb_t *p, const uint8_t *file, uint32_t size,
             const char *path, const char *const *argv);

#endif /* PPAP_EXEC_EXEC_CPM_H */
