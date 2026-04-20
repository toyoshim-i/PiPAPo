/*
 * dos_host.h — MS-DOS subsystem memory image setup interface
 */

#ifndef PPAP_KERNEL_CORE_SUBSYS_MSDOS_DOS_HOST_H
#define PPAP_KERNEL_CORE_SUBSYS_MSDOS_DOS_HOST_H

#include <stdint.h>

#include "kernel/common/vfs/vfs_types.h"
#include "kernel/core/mm/page.h"

/* Minimum DOS run = 17 contiguous 4 KB pages = 68 KB.  The first 16
 * bytes (paragraph 0) hold the process's MCB header; the next 64 KB
 * are the proc_seg window that DOS programs address directly.  A .COM
 * expects SP=0xFFFE and therefore needs the full 64 KB past the MCB
 * paragraph; 17 pages is the exact floor (16 B MCB + 65536 B window =
 * 65552 B, which fits in 17 * 4096 = 69632 B with 4080 B of slack at
 * the run tail). */
#define DOS_SEG_PAGES 17u
#define DOS_SEG_BYTES ((uint32_t)DOS_SEG_PAGES * PAGE_SIZE)

/* Cap for the largest contiguous run we'll grab for a DOS binary.
 * Sized to fit common DOS apps (zork1 needs ~20 pages = 80 KB) while
 * leaving comfortable headroom for kernel/VFS allocations on the
 * limited pcxt page pool (~120 pages total).  The extra space past
 * the proc_seg window is reachable through AH=48h-allocated MCBs. */
#define DOS_SEG_PAGES_MAX 32u

/* MCB header layout (16 bytes = 1 paragraph), preceding each block.
 *   +0  sig   'M' (more) or 'Z' (last)
 *   +1  owner PSP segment of owning process (0 = free)
 *   +3  size  payload paragraphs (not counting this MCB header)
 *   +5  ..    11 bytes reserved (zeros on our side) */
#define DOS_MCB_SIG_M 0x4Du
#define DOS_MCB_SIG_Z 0x5Au
#define DOS_MCB_OFF_SIG 0x00u
#define DOS_MCB_OFF_OWNER 0x01u
#define DOS_MCB_OFF_SIZE 0x03u
#define DOS_MCB_BYTES 0x10u

/* Max .COM size: 64 KB - 256 (PSP) - 1 */
#define DOS_COM_MAX_SIZE 0xFEFFu

/*
 * dos_build_com_image — build the MCB header + PSP, stream the .COM
 * binary from the given vnode, and construct the initial user-mode
 * stack frame.
 *
 * Layout inside the run starting at base_id (proc_seg = base_id/16 + 1):
 *   base_id:0000   MCB 'Z' (16 B, owner=proc_seg)
 *   proc_seg:0000  PSP (256 B: INT 20h, mem top, INT 21h+RETF, cmdline)
 *   proc_seg:0100  .COM binary (streamed via mod_vfs.vnode_read)
 *   proc_seg:FFxx  User stack frame (HW + SW, matches trap.S restore)
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
                        const char *const *argv, const char *const *envp,
                        uint16_t *out_user_sp);

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
 * dos_build_exe_image — build the MCB header + PSP, stream the MZ
 * image payload, apply relocations, and construct the initial
 * user-mode stack frame for a .EXE binary.
 *
 * Layout inside the run starting at base_id (proc_seg = base_id/16 + 1,
 * load_seg = proc_seg + 0x10):
 *   base_id:0000   MCB 'Z' (16 B, owner=proc_seg)
 *   proc_seg:0000  PSP (256 B)
 *   load_seg:0000  Image payload (file offset hdr->header_size * 16)
 *   (hdr->init_ss + load_seg):(hdr->init_sp - 24 .. hdr->init_sp)
 *                  HW + SW initial frame (matches trap.S restore path)
 *
 * On entry:
 *   CS = hdr->init_cs + load_seg,  IP = hdr->init_ip
 *   SS = hdr->init_ss + load_seg,  SP = initial frame top
 *   DS = ES = proc_seg, FLAGS = 0x0200 (IF=1)
 *
 * Returns 0 on success with *out_user_ss / *out_user_sp populated, or a
 * negative errno.
 */
int dos_build_exe_image(page_id_t base_id, uint16_t proc_seg,
                        uint32_t seg_pages, vnode_t *vn, uint32_t file_size,
                        const mz_header_t *hdr, const char *const *argv,
                        const char *const *envp, uint16_t *out_user_ss,
                        uint16_t *out_user_sp);

#endif /* PPAP_KERNEL_CORE_SUBSYS_MSDOS_DOS_HOST_H */
