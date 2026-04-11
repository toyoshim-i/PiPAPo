/*
 * pcxt_vfs_init.c — PC/XT VFS-side platform init
 *
 * Overrides the weak vfs_notify to handle pcxt-specific events:
 *  - MODULE_READY: logger + BIOS console backend
 *  - LATE_INIT: block device init
 *  - IDLE: tty input polling
 */

#include "kernel/common/mod/mod_vfs.h"
#include "kernel/vfs/driver/bios_blk.h"
#include "kernel/vfs/driver/bios_con.h"
#include "kernel/vfs/driver/blkdev.h"
#include "kernel/vfs/klog.h"
#include "kernel/vfs/tty.h"

void vfs_notify(int event) {
  switch (event) {
    case VFS_EVENT_MODULE_READY:
      klog_init_logger();
      tty_set_backend(TTY_DISPLAY, &bios_con_backend);
      tty_set_console(TTY_DISPLAY);
      break;
    case VFS_EVENT_LATE_INIT:
      blkdev_init();
      bios_blk_init();
      break;
    case VFS_EVENT_IDLE:
      tty_poll_input();
      break;
  }
}
