/*
 * kbd.h — PicoCalc keyboard driver (STM32 co-processor via I2C1)
 *
 * Polls the STM32 keyboard controller for key events and translates
 * them to ASCII / VT100 escape sequences for terminal input.
 */

#ifndef PPAP_DRIVERS_KBD_H
#define PPAP_DRIVERS_KBD_H

/* Initialise keyboard: verify STM32 presence, drain stale FIFO. */
void kbd_init(void);

/* Returns non-zero if the STM32 keyboard was detected during kbd_init(). */
int kbd_present(void);

/* Poll for one input byte.  Returns a character (0x00–0xFF) or -1 if
 * no input is available.  Multi-byte escape sequences (e.g. arrow keys)
 * are buffered internally and returned one byte per call. */
int kbd_poll(void);

/* Returns non-zero if keyboard input is available (buffered escape sequence
 * bytes or FIFO entries from the STM32).  Does not consume any data. */
int kbd_poll_avail(void);

#endif /* PPAP_DRIVERS_KBD_H */