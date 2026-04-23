/*
 * ppap_m68k_host.h — PPAP-on-m68k-emulator image setup interface
 */

#ifndef PPAP_KERNEL_CORE_SUBSYS_PPAP_PPAP_M68K_HOST_H
#define PPAP_KERNEL_CORE_SUBSYS_PPAP_PPAP_M68K_HOST_H

#include <stdint.h>

#include "kernel/core/cpu/ecpu_m68k.h"

struct exec_args;

/*
 * ppap_m68k_load_elf — load a m68k ELF image into emulated memory and
 * set up the initial user stack with argc/argv/envp.
 *
 * The caller must pre-allocate emu_mem of emu_mem_size bytes and
 * initialize the m68k emulator state.  This function:
 *   - Iterates PT_LOAD segments and copies file data; zeros BSS
 *   - Sets the m68k PC to e_entry
 *   - Pushes argument strings (read out of `args` via the
 *     exec_args_* accessors), pointer tables, and argc onto the
 *     emulated stack (at the top of emu_mem), assigns SP to a[7]
 *
 * Returns 0 on success.
 */
int ppap_m68k_load_elf(m68k_state_t *cpu, uint8_t *emu_mem,
                       uint32_t emu_mem_size, const uint8_t *file_buf,
                       uint32_t file_size, const struct exec_args *args,
                       uint32_t *entry_out);

#endif /* PPAP_KERNEL_CORE_SUBSYS_PPAP_PPAP_M68K_HOST_H */
