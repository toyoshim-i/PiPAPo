/*
 * human68k_host.h — Human68k subsystem memory image setup interface
 *
 * Declares common loader setup functions for Human68k X-format (.x) and
 * R-format (.r) binaries.
 */

#ifndef PPAP_KERNEL_CORE_SUBSYS_HUMAN68K_HUMAN68K_HOST_H
#define PPAP_KERNEL_CORE_SUBSYS_HUMAN68K_HUMAN68K_HOST_H

#include <stdint.h>

/*
 * human68k_setup_pmb — initialize Process Memory Block fields.
 *
 * Sets up the PMB structure at the start of the allocated memory block,
 * including:
 *   - MMB header (prev/owner/block_end/next)
 *   - PMB environment, command-line, file handles, segment addresses
 *   - Execution path and filename
 *
 * Arguments:
 *   base        — pointer to allocated memory block
 *   total_bytes — total size of allocated block
 *   image_size  — size of loaded image (text + data + bss)
 *   path        — full path to executable (for path field at 0x82/0xC4)
 */
void human68k_setup_pmb(uint8_t *base, uint32_t total_bytes,
                        uint32_t image_size, const char *path);

/*
 * human68k_setup_registers — patch initial CPU registers in stack frame.
 *
 * Initializes register values (A0–A4) in the software interrupt frame
 * according to the Human68k protocol (§4.3):
 *   A0 = PMB base
 *   A1 = memory end + 1
 *   A2 = command-line address
 *   A3 = environment (-1 = none)
 *   A4 = entry point (text base for X-format, image start for R-format)
 *
 * Arguments:
 *   sp          — stack pointer (kernel SSP) from p->sp
 *   pmb_base    — base address of PMB
 *   block_end   — end of allocated block
 *   cmdline_ptr — address of command-line buffer (typically pmb_base + 0x6C)
 *   entry_ptr   — entry point address
 */
void human68k_setup_registers(uint32_t sp, uint32_t pmb_base,
                              uint32_t block_end, uint32_t cmdline_ptr,
                              uint32_t entry_ptr);

#endif /* PPAP_KERNEL_CORE_SUBSYS_HUMAN68K_HUMAN68K_HOST_H */
