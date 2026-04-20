/*
 * human68k_host.h — Human68k subsystem memory image setup interface
 *
 * Declares common loader setup functions for Human68k X-format (.x) and
 * R-format (.r) binaries.
 */

#ifndef PPAP_KERNEL_CORE_SUBSYS_HUMAN68K_HUMAN68K_HOST_H
#define PPAP_KERNEL_CORE_SUBSYS_HUMAN68K_HUMAN68K_HOST_H

#include <stdint.h>

#include "kernel/core/mm/page.h"

/*
 * human68k_build_env — allocate and populate an env block from envp.
 *
 * On success, stores a freshly-allocated page's linear address in
 * *out_env_addr and its page_id in *out_env_page.  Layout:
 *
 *     [4-byte BE size]      (payload size, including trailing '\0')
 *     [NAME1=VALUE1\0]
 *     [NAME2=VALUE2\0]
 *     ...
 *     [\0]                  (empty-string terminator)
 *
 * If envp is NULL or empty, sets *out_env_addr = 0xFFFFFFFF and
 * *out_env_page = PAGE_ID_INVALID — equivalent to Human68k's "no env"
 * sentinel, matching a3 = -1 at process entry.
 *
 * Returns 0 on success, negative errno on allocation failure.  Callers
 * own the returned page and should proc_track_page() it so it's freed
 * on process exit.
 */
int human68k_build_env(const char *const *envp, page_id_t *out_env_page,
                       uint32_t *out_env_addr);

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
 *   env_addr    — linear address of env block, or 0xFFFFFFFF for none
 *                 (as returned by human68k_build_env)
 */
void human68k_setup_pmb(uint8_t *base, uint32_t total_bytes,
                        uint32_t image_size, const char *path,
                        uint32_t env_addr);

/*
 * human68k_setup_registers — patch initial CPU registers in stack frame.
 *
 * Initializes register values (A0–A4) in the software interrupt frame
 * according to the Human68k protocol (§4.3):
 *   A0 = PMB base
 *   A1 = memory end + 1
 *   A2 = command-line address
 *   A3 = environment address (0xFFFFFFFF = none)
 *   A4 = entry point (text base for X-format, image start for R-format)
 *
 * Arguments:
 *   sp          — stack pointer (kernel SSP) from p->sp
 *   pmb_base    — base address of PMB
 *   block_end   — end of allocated block
 *   cmdline_ptr — address of command-line buffer (typically pmb_base + 0x6C)
 *   env_addr    — env block address (or 0xFFFFFFFF for none)
 *   entry_ptr   — entry point address
 */
void human68k_setup_registers(uint32_t sp, uint32_t pmb_base,
                              uint32_t block_end, uint32_t cmdline_ptr,
                              uint32_t env_addr, uint32_t entry_ptr);

#endif /* PPAP_KERNEL_CORE_SUBSYS_HUMAN68K_HUMAN68K_HOST_H */
