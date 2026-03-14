/*
 * target_default.c — Weak default implementations of optional target hooks.
 *
 * Each function here is a weak symbol that can be overridden by a target's
 * target_<name>.c.  Targets that do not need a particular hook simply inherit
 * the do-nothing default defined here.
 */

#include "target.h"

/*
 * Default target_mount_rootfs(): no target-specific rootfs.
 * Overridden by targets that load a rootfs from an external source
 * (e.g. x68k loads a UFS image from RAM placed there by stage2).
 */
__attribute__((weak)) int target_mount_rootfs(void)
{
    return -1;
}
