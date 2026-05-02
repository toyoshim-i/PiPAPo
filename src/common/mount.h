/*
 * mount.h --- mount(2) / umount2(2) flag constants
 *
 * Shared between kernel and user space.
 *
 * PPAP currently honours only MS_RDONLY among the Linux mount-flag
 * set; other bits are accepted by the syscall but ignored.  Kept
 * stable here so user-space tools (mount, umount) and kernel callers
 * agree on the bit layout.
 */

#ifndef PPAP_COMMON_MOUNT_H
#define PPAP_COMMON_MOUNT_H

#define MS_RDONLY 1u

/* Linux umount2(2) flag — accepted for ABI compatibility but PPAP
 * always performs a synchronous unmount; force / lazy semantics are
 * not implemented. */
#define MNT_FORCE 0x00000001u

#endif /* PPAP_COMMON_MOUNT_H */
