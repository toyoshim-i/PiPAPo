/*
 * cc_kbd.h — M5Stack CardComputer keyboard driver
 *
 * Direct-GPIO matrix scanner for the 56-key membrane keyboard
 * (3-bit counter + external 1-of-8 demultiplexer + 7 INPUT_PULLUP
 * lines decoded into a 4×14 logical keymap).  See
 * docs/reference/cardcomputer.md §Keyboard for the full topology.
 *
 * The API mirrors src/kernel/vfs/driver/i2c_kbd.h on pico1calc so
 * that target glue code (xtensa_cc_logger.c) can reuse the same
 * ring-buffer drain pattern.
 */

#ifndef PPAP_TARGET_XTENSA_CC_KERNEL_VFS_DRIVER_CC_KBD_H
#define PPAP_TARGET_XTENSA_CC_KERNEL_VFS_DRIVER_CC_KBD_H

/* Configure the 3 counter outputs and 7 input lines.  Idempotent. */
void kbd_init(void);

/* Poll for one input byte.  Returns 0x00–0xFF on success or -1 if
 * no input is available.  Multi-byte VT100 escape sequences (arrow
 * keys, F-keys, Esc, Delete) are buffered internally and returned
 * one byte per call. */
int kbd_poll(void);

/* Returns non-zero if keyboard input is available (buffered escape
 * bytes or a newly-pressed non-modifier key in the current scan).
 * Does not consume any data. */
int kbd_poll_avail(void);

#endif /* PPAP_TARGET_XTENSA_CC_KERNEL_VFS_DRIVER_CC_KBD_H */
