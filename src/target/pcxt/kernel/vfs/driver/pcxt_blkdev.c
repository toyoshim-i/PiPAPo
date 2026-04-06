/*
 * pcxt_blkdev.c — PC/XT block device registration (VFS side)
 *
 * Registers the floppy block device during VFS_EVENT_LATE_INIT.
 * Overrides the weak vfs_notify to add LATE_INIT handling while
 * preserving the default MODULE_READY and IDLE behaviour.
 */

#include "kernel/common/mod/mod_vfs.h"
#include "kernel/vfs/driver/blkdev.h"
#include "kernel/vfs/driver/floppy_blk.h"
#include "kernel/vfs/klog.h"
#include "kernel/vfs/tty.h"

void vfs_notify(int event) {
  switch (event) {
    case VFS_EVENT_MODULE_READY:
      klog_init_logger();
      break;
    case VFS_EVENT_LATE_INIT:
      blkdev_init();
      floppy_blk_init();
      break;
    case VFS_EVENT_IDLE:
      tty_poll_input();
      break;
  }
}
