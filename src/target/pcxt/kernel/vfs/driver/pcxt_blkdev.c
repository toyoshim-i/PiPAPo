/*
 * pcxt_blkdev.c — PC/XT VFS-side platform init
 *
 * Overrides the weak vfs_notify to handle pcxt-specific events:
 *  - MODULE_READY: logger
 *  - LATE_INIT: floppy block device + BIOS console backend
 *  - IDLE: tty input polling
 */

#include "kernel/common/mod/mod_vfs.h"
#include "kernel/vfs/driver/bios_blk.h"
#include "kernel/vfs/driver/bios_con.h"
#include "kernel/vfs/driver/blkdev.h"
#include "kernel/vfs/klog.h"
#include "kernel/vfs/tty.h"

/* ── BIOS console backend ──────────────────────────────────────────────────
 * INT 16h keyboard input with BIOS scan code → VT100 translation.
 */

static int bios_con_putc(char c, void (*notify)(void)) {
  (void)notify;
  bios_putc(c);
  return 1;
}

/* Translate BIOS scan codes (AL=0) to VT100 escape sequences.
 * Since getc returns one char at a time, we buffer the tail bytes. */
static char bios_esc_buf[3];
static unsigned char bios_esc_pos;
static unsigned char bios_esc_len;

static int bios_con_getc(void) {
  /* Drain any pending escape sequence bytes first */
  if (bios_esc_pos < bios_esc_len)
    return (unsigned char)bios_esc_buf[bios_esc_pos++];

  /* BIOS INT 16h AH=01h: check if keystroke available (non-blocking) */
  unsigned short flags;
  unsigned short ax;
  __asm__ volatile(
      "push %%ds\n\t"
      "push %%es\n\t"
      "mov $0x0100, %%ax\n\t"
      "int $0x16\n\t"
      "pushf\n\t"
      "pop %0\n\t"
      "mov %%ax, %1\n\t"
      "pop %%es\n\t"
      "pop %%ds"
      : "=r"(flags), "=r"(ax)
      :
      : "ax", "cc", "memory");
  if (flags & 0x0040u) return -1; /* ZF set = no key */

  /* BIOS INT 16h AH=00h: read the keystroke (consume it) */
  __asm__ volatile(
      "push %%ds\n\t"
      "push %%es\n\t"
      "mov $0x0000, %%ax\n\t"
      "int $0x16\n\t"
      "mov %%ax, %0\n\t"
      "pop %%es\n\t"
      "pop %%ds"
      : "=r"(ax)
      :
      : "ax", "cc", "memory");

  unsigned char al = (unsigned char)(ax & 0xFFu);
  unsigned char ah = (unsigned char)(ax >> 8);

  if (al != 0) return al; /* Normal ASCII key */

  /* Extended key: translate scan code → VT100 ESC [ <letter> */
  char letter;
  switch (ah) {
    case 0x48: letter = 'A'; break; /* Up    */
    case 0x50: letter = 'B'; break; /* Down  */
    case 0x4D: letter = 'C'; break; /* Right */
    case 0x4B: letter = 'D'; break; /* Left  */
    case 0x47: letter = 'H'; break; /* Home  */
    case 0x4F: letter = 'F'; break; /* End   */
    case 0x53: letter = '~'; break; /* Delete (ESC [ 3 ~) */
    default: return -1; /* Ignore other extended keys */
  }

  if (ah == 0x53) {
    /* Delete: ESC [ 3 ~ */
    bios_esc_buf[0] = '[';
    bios_esc_buf[1] = '3';
    bios_esc_buf[2] = '~';
    bios_esc_len = 3;
  } else {
    bios_esc_buf[0] = '[';
    bios_esc_buf[1] = letter;
    bios_esc_len = 2;
  }
  bios_esc_pos = 0;
  return 0x1B; /* ESC — caller will get '[' and letter on next calls */
}

static int bios_con_rx_avail(void) {
  /* Buffered escape sequence bytes are immediately available */
  if (bios_esc_pos < bios_esc_len) return 1;
  /* Direct BDA read: keyboard buffer head/tail at 0x0040:0x001A/0x001C */
  unsigned short head;
  unsigned short tail;
  __asm__ volatile(
      "push %%es\n\t"
      "mov $0x0040, %%ax\n\t"
      "mov %%ax, %%es\n\t"
      "mov %%es:0x1A, %0\n\t"
      "mov %%es:0x1C, %1\n\t"
      "pop %%es"
      : "=r"(head), "=r"(tail)
      :
      : "ax", "memory");
  return head != tail ? 1 : 0;
}

static const tty_backend_t bios_con_backend = {
    .putc = bios_con_putc,
    .flush = NULL,
    .getc = bios_con_getc,
    .rx_avail = bios_con_rx_avail,
    .get_cols = NULL,
    .get_rows = NULL,
    .set_winsize = NULL,
};

/* ── VFS event handler ─────────────────────────────────────────────────── */

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
