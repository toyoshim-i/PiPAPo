/*
 * dos_host.h — MS-DOS subsystem memory image setup interface
 */

#ifndef PPAP_KERNEL_CORE_SUBSYS_MSDOS_DOS_HOST_H
#define PPAP_KERNEL_CORE_SUBSYS_MSDOS_DOS_HOST_H

#include <stdint.h>

#include "kernel/common/vfs/vfs_types.h"
#include "kernel/core/mm/page.h"

/* 64 KB DOS segment = 16 contiguous 4 KB pages */
#define DOS_SEG_PAGES 16u
#define DOS_SEG_BYTES ((uint32_t)DOS_SEG_PAGES * PAGE_SIZE)
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
int dos_build_com_image(page_id_t base_id, uint16_t proc_seg, vnode_t *vn,
                        uint32_t file_size, const char *const *argv,
                        uint16_t *out_user_sp);

#endif /* PPAP_KERNEL_CORE_SUBSYS_MSDOS_DOS_HOST_H */
