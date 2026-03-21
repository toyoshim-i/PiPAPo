/*
 * h68k_emu.h — Shared Human68k m68k emulator declarations
 *
 * Declares types and functions shared by X-format and R-format loaders
 * when running Human68k binaries on non-m68k hosts via the m68k emulator.
 */

#ifndef PPAP_KERNEL_EXEC_H68K_EMU_H
#define PPAP_KERNEL_EXEC_H68K_EMU_H

#if !defined(__m68k__)

#include <stdint.h>
#include "kernel/cpu/cpu.h"
#include "kernel/proc/proc.h"
#include "kernel/cpu/ecpu_m68k.h"

/* Maximum emulator memory pages (one page reserved for state struct) */
#define H68K_EMU_MEM_PAGES_MAX  (USER_PAGES_MAX - 1u)

/* Maximum concurrent malloc() blocks tracked */
#define H68K_EMU_MALLOC_MAX     8u

/* MMB header size prepended to each malloc block */
#define H68K_MMB_HEADER_SIZE    0x10u

/* PMB size: 256 bytes at the start of the process memory block */
#define X68K_PMB_SIZE  0x100

/*
 * Per-process Human68k emulator execution state.
 * Stored in the last page of the allocated contiguous block.
 */
typedef struct {
    struct {
        uint32_t base;
        uint32_t nbytes;
    } mallocs[H68K_EMU_MALLOC_MAX];
    m68k_state_t m68k;
    int32_t exit_code;
    uint32_t heap_next;
    uint32_t block_end;
} h68k_emu_exec_state_t;

/*
 * h68k_emu_trap_handler — Human68k DOS call trap handler for m68k emulator.
 *
 * Intercepts F-line (0xFFxx) illegal instructions and translates them
 * into PPAP syscalls.  ctx must point to h68k_emu_exec_state_t.
 */
int h68k_emu_trap_handler(cpu_state_t *state, int trap_type,
                          uint32_t param, void *ctx);

/*
 * h68k_emu_run_process — kernel-mode entry point for emulated Human68k processes.
 *
 * Called via proc_setup_stack(); runs the m68k emulator loop and calls
 * sys_exit() when the guest program terminates.
 */
void h68k_emu_run_process(void);

#endif /* !defined(__m68k__) */
#endif /* PPAP_KERNEL_EXEC_H68K_EMU_H */
