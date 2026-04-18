/*
 * dos_host.h — MS-DOS subsystem memory image setup interface
 */

#ifndef PPAP_KERNEL_CORE_SUBSYS_MSDOS_DOS_HOST_H
#define PPAP_KERNEL_CORE_SUBSYS_MSDOS_DOS_HOST_H

#include <stdint.h>

#include "kernel/common/vfs/vfs_types.h"
#include "kernel/core/mm/page.h"

/* Minimum DOS segment = 64 KB = 16 contiguous 4 KB pages.  This is the
 * floor every COM gets; the loader tries to grab as much contiguous RAM
 * as is available up to DOS_SEG_PAGES_MAX so apps that walk into "their
 * own" high memory (zork1, etc.) find it backed. */
#define DOS_SEG_PAGES 16u
#define DOS_SEG_BYTES ((uint32_t)DOS_SEG_PAGES * PAGE_SIZE)

/* Cap for the largest contiguous run we'll grab for a COM.  Sized to fit
 * ~all common DOS apps (zork1 needs ~20 pages = 80 KB) while leaving
 * comfortable headroom for kernel/VFS allocations on the limited pcxt
 * page pool (~120 pages total). */
#define DOS_SEG_PAGES_MAX 32u

/* Max .COM size: 64 KB - 256 (PSP) - 1 */
#define DOS_COM_MAX_SIZE 0xFEFFu

/*
 * dos_build_com_image — build the PSP, stream the .COM binary from the
 * given vnode, and construct the initial user-mode stack frame.
 *
 * Layout inside the 64 KB segment starting at base_id:
 *   seg:0000  PSP (256 bytes, with INT 20h, mem top, INT 21h+RETF, cmdline)
 *   seg:0100  .COM binary (streamed via mod_vfs.vnode_read)
 *   seg:FFxx  User stack frame (HW + SW, matches trap.S restore path)
 *
 * On entry:
 *   CS = DS = ES = SS = proc_seg
 *   IP = 0x0100, SP = points at SW frame (just below HW frame)
 *   FLAGS = 0x0200 (IF=1)
 *
 * Returns 0 on success with *out_user_sp populated, or a negative errno.
 */
int dos_build_com_image(page_id_t base_id, uint16_t proc_seg,
                        uint32_t seg_pages, vnode_t *vn, uint32_t file_size,
                        const char *const *argv, uint16_t *out_user_sp);

/* MS-DOS .EXE (MZ) header, 28 bytes on disk, little-endian. */
typedef struct mz_header {
  uint16_t signature; /* 'MZ' = 0x5A4D (or 'ZM' = 0x4D5A) */
  uint16_t last_page_size;
  uint16_t page_count;
  uint16_t reloc_count;
  uint16_t header_size; /* in paragraphs */
  uint16_t min_alloc;   /* min paragraphs past image */
  uint16_t max_alloc;   /* max paragraphs past image */
  uint16_t init_ss;     /* relative — add load_seg */
  uint16_t init_sp;
  uint16_t checksum;
  uint16_t init_ip;
  uint16_t init_cs; /* relative — add load_seg */
  uint16_t reloc_offset;
  uint16_t overlay;
} mz_header_t;

/*
 * dos_build_exe_image — build the PSP, stream the MZ image payload, and
 * construct the initial user-mode stack frame for a .EXE binary.
 *
 * Layout inside the run starting at base_id (seg = proc_seg = psp_seg):
 *   seg:0000          PSP (256 bytes)
 *   seg:0100          Image payload (streamed from file offset
 *                     hdr->header_size * 16)
 *   (init_ss+0x10):(init_sp - 24..init_sp)
 *                     HW + SW initial frame (matches trap.S restore path)
 *
 * On entry:
 *   CS = hdr->init_cs + load_seg,  IP = hdr->init_ip
 *   SS = hdr->init_ss + load_seg,  SP = initial frame top
 *   DS = ES = proc_seg, FLAGS = 0x0200 (IF=1)
 *
 * Relocations (hdr->reloc_count != 0) are not applied in D-4a; the
 * loader rejects such files before this function is called.
 *
 * Returns 0 on success with *out_user_ss / *out_user_sp populated, or a
 * negative errno.
 */
int dos_build_exe_image(page_id_t base_id, uint16_t proc_seg,
                        uint32_t seg_pages, vnode_t *vn, uint32_t file_size,
                        const mz_header_t *hdr, const char *const *argv,
                        uint16_t *out_user_ss, uint16_t *out_user_sp);

#endif /* PPAP_KERNEL_CORE_SUBSYS_MSDOS_DOS_HOST_H */
