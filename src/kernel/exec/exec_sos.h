/*
 * exec_sos.h — S-OS "SWORD" .obj binary loader
 *
 * Detects S-OS binaries by _SOS magic header and loads them into a Z80
 * emulator instance for execution.  The S-OS bridge handles system calls
 * via the eCPU trap mechanism (RST 18h).
 *
 * See docs/proposals/sos_subsystem.md for the design.
 */

#ifndef PPAP_EXEC_EXEC_SOS_H
#define PPAP_EXEC_EXEC_SOS_H

#include "kernel/proc/proc.h"
#include <stdint.h>

/*
 * sos_detect — check if a file is an S-OS "SWORD" binary.
 *
 * Detection is by _SOS magic at offset 0 (4 bytes: 0x5F 0x53 0x4F 0x53)
 * and LF terminator at offset 17.
 *
 * Returns 1 if S-OS, 0 otherwise.
 */
int sos_detect(const char *path, const uint8_t *file, uint32_t size);

/*
 * exec_sos — load and set up an S-OS binary for Z80 emulation.
 *
 * Parses the 18-byte _SOS header to extract load and execution addresses,
 * allocates 64KB for the Z80 address space, initialises the S-OS memory
 * map (work area, RST vectors), and loads the payload at the
 * header-specified address.
 *
 * Returns 0 on success, negative errno on failure.
 */
int exec_sos(pcb_t *p, const uint8_t *file, uint32_t size,
             const char *path, const char *const *argv);

#endif /* PPAP_EXEC_EXEC_SOS_H */
