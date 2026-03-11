/*
 * exec_m68k_emu.h — m68k ELF binary loader for cross-arch emulation
 *
 * Detects m68k ELF binaries (EM_68K, big-endian) on non-m68k hosts
 * and loads them into an eCPU-m68k emulator instance for execution.
 * The PPAP cross-arch personality handles TRAP #0 → PPAP syscalls.
 *
 * See docs/ecpu-m68k.md §Step 8 for the full design.
 */

#ifndef PPAP_EXEC_M68K_EMU_H
#define PPAP_EXEC_M68K_EMU_H

#include "kernel/proc/proc.h"
#include "elf.h"
#include <stdint.h>

/*
 * m68k_emu_detect — check if an ELF is a m68k binary on a non-m68k host.
 *
 * Returns 1 if the binary is EM_68K and we're running on a non-m68k
 * architecture (i.e., needs emulation), 0 otherwise.
 */
int m68k_emu_detect(const elf32_ehdr_t *ehdr);

/*
 * exec_m68k_emu — load an m68k ELF binary for emulated execution.
 *
 * Allocates emulated memory (1MB), copies PT_LOAD segments (with
 * byte order conversion where needed), sets up user stack with
 * argc/argv, and initialises the eCPU-m68k state with the PPAP
 * cross-arch trap handler.
 *
 * Returns 0 on success, negative errno on failure.
 */
int exec_m68k_emu(pcb_t *p, const uint8_t *file_base, uint32_t file_size,
                   const elf32_ehdr_t *ehdr, const char *path,
                   const char *const *argv);

#endif /* PPAP_EXEC_M68K_EMU_H */
