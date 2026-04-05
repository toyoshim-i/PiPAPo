/*
 * cpm_loader.h — CP/M .COM binary loader interface
 *
 * Declares the loader setup and public entry point cpm_load_com() which
 * initializes the Z80 memory, CP/M state, and binary image.
 */

#ifndef PPAP_KERNEL_SUBSYS_CPM_LOADER_H
#define PPAP_KERNEL_SUBSYS_CPM_LOADER_H

#include "kernel/core/subsys/cpm_bridge.h"

/*
 * cpm_load_com — load a .COM binary into Z80 memory with CP/M setup.
 *
 * Initializes the Z80 address space with the CP/M 2.2 memory map:
 *   - Zero page (0x0000–0x00FF): CP/M vectors and IOBYTE
 *   - FCB area (0x005C–0x007F): default FCBs and DMA buffer
 *   - TPA (0x0100–0xFE00): loaded .COM binary
 *   - BIOS table (0xFE00–0xFFFF): BIOS jump table (trap stubs)
 *
 * Also sets up the initial Z80 state (PC, SP) and initializes cpm_state_t
 * with default drive, user, and open file table.
 *
 * Arguments:
 *   cpu      — initialized Z80 emulator state (memory must be allocated)
 *   cpm      — CP/M process state (will be initialized)
 *   binary   — .COM binary image
 *   size     — size of binary (will be clamped to TPA size)
 *   cmdline  — command-line arguments (parsed into FCBs and DMA buffer)
 */
void cpm_load_com(z80_state_t *cpu, cpm_state_t *cpm, const uint8_t *binary,
                  uint32_t size, const char *cmdline);

#endif /* PPAP_KERNEL_SUBSYS_CPM_LOADER_H */
