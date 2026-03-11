/*
 * cpm_bridge.h — CP/M 2.2 subsystem state and constants
 *
 * Defines the per-process CP/M state (cpm_state_t), memory map addresses,
 * and the public API used by the loader and trap handler.
 *
 * See docs/subsystem-cpm.md for the full design.
 */

#ifndef PPAP_SUBSYS_CPM_BRIDGE_H
#define PPAP_SUBSYS_CPM_BRIDGE_H

#include <stdint.h>
#include "kernel/ecpu/ecpu.h"
#include "kernel/ecpu/ecpu_z80.h"

/* ── CP/M memory map constants ──────────────────────────────────────────── */
#define CPM_TPA_BASE       0x0100  /* .COM programs load here              */
#define CPM_TPA_END        0xFE00  /* end of Transient Program Area        */
#define CPM_MAX_COM_SIZE   (CPM_TPA_END - CPM_TPA_BASE)

#define CPM_BDOS_ENTRY     0x0005  /* CALL 5 → BDOS dispatcher             */
#define CPM_BIOS_ENTRY     0xFE00  /* BIOS jump table base                 */
#define CPM_BIOS_SIZE      (17 * 3)  /* 17 functions × 3 bytes (JP addr)  */

#define CPM_DMA_DEFAULT    0x0080  /* default DMA buffer address           */
#define CPM_FCB1_ADDR      0x005C  /* default FCB 1                        */
#define CPM_FCB2_ADDR      0x006C  /* default FCB 2                        */

#define CPM_MAX_OPEN_FILES 8       /* max simultaneous open files          */

/* ── Per-process CP/M state ─────────────────────────────────────────────── */
typedef struct cpm_state {
    uint8_t  current_drive;    /* 0=A, 1=B, ..., 15=P                     */
    uint8_t  current_user;     /* 0–15                                     */
    uint16_t dma_addr;         /* DMA buffer address (default 0x0080)      */

    /* FCB-to-fd mapping table */
    struct {
        uint16_t fcb_addr;     /* Z80 address of FCB (0 = free slot)       */
        int      fd;           /* host/PPAP file descriptor                */
        uint32_t file_pos;     /* current file position in bytes           */
    } open_files[CPM_MAX_OPEN_FILES];
} cpm_state_t;

/* ── Trap handler (personality layer) ───────────────────────────────────── */

/*
 * cpm_trap_handler — intercepts CALL instructions to BDOS/BIOS entry points.
 *
 * Register as the Z80 core's trap handler via set_trap_handler().
 * The ctx parameter must point to a cpm_state_t.
 */
int cpm_trap_handler(ecpu_state_t *cpu, int trap_type,
                     uint32_t param, void *ctx);

/* ── Loader ─────────────────────────────────────────────────────────────── */

/*
 * cpm_load_com — load a .COM binary into Z80 memory and set up the
 *                CP/M memory map (zero page, BIOS stubs, DMA, FCBs).
 *
 * @cpu:      initialized z80_state_t (cast to ecpu_state_t)
 * @cpm:      per-process CP/M state (zeroed by caller)
 * @binary:   pointer to raw .COM file data
 * @size:     size of binary in bytes
 * @cmdline:  command-line tail (NULL for none)
 */
void cpm_load_com(z80_state_t *cpu, cpm_state_t *cpm,
                  const uint8_t *binary, uint32_t size,
                  const char *cmdline);

#endif /* PPAP_SUBSYS_CPM_BRIDGE_H */
