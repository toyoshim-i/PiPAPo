/*
 * cpm_bridge.h — CP/M 2.2 subsystem state and constants
 *
 * Defines the per-process CP/M state (cpm_state_t), memory map addresses,
 * and the public API used by the loader and trap handler.
 *
 * See docs/subsystems/cpm.md for the full design.
 */

#ifndef PPAP_SUBSYS_CPM_BRIDGE_H
#define PPAP_SUBSYS_CPM_BRIDGE_H

#include <stdint.h>
#include "config.h"
#include "kernel/ecpu/ecpu.h"
#include "kernel/ecpu/ecpu_z80.h"
#include "subsys.h"

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
#define CPM_RECORD_SIZE    128     /* CP/M record size in bytes            */

/* ── Per-process CP/M state ─────────────────────────────────────────────── */
typedef struct cpm_state {
    uint8_t  current_drive;    /* 0=A, 1=B, ..., 15=P                     */
    uint8_t  current_user;     /* 0–15                                     */
    uint16_t dma_addr;         /* DMA buffer address (default 0x0080)      */
    char     drive_a_root[VFS_PATH_MAX]; /* host/PPAP path mapped to A:     */

    /* FCB-to-fd mapping table */
    struct {
        uint16_t fcb_addr;     /* Z80 address of FCB (0 = free slot)       */
        int      fd;           /* host/PPAP file descriptor                */
        uint32_t file_pos;     /* current file position in bytes           */
    } open_files[CPM_MAX_OPEN_FILES];

    /* Directory search state (for BDOS fn 17/18) */
    void    *search_dir;       /* DIR* (host) or opaque dir handle         */
    uint8_t  search_pattern[12]; /* 8+3 filename pattern from search FCB   */
    uint8_t  search_drive;     /* drive being searched                     */
} cpm_state_t;

/* ── Trap handler (personality layer) ───────────────────────────────────── */

int cpm_trap_handler(ecpu_state_t *cpu, int trap_type,
                     uint32_t param, void *ctx);

/* ── FCB utilities (public for testing) ─────────────────────────────────── */

void cpm_fcb_to_path(cpm_state_t *cpm, const uint8_t *fcb,
                     char *path, int path_size);

int cpm_match_fcb(const uint8_t *pattern, const char *filename);

/* Kernel-mode entry point for CP/M processes (called via proc_setup_stack) */
void cpm_run_process(void);

/* Subsystem ops for CP/M — registered into subsys_ops_table[] */
extern const subsys_ops_t cpm_subsys_ops;

#endif /* PPAP_SUBSYS_CPM_BRIDGE_H */
