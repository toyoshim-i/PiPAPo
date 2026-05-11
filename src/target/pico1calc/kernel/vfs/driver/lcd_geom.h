/*
 * lcd_geom.h — PicoCalc LCD geometry
 *
 * Per-target visible-window dimensions consumed by the generic LCD
 * driver (kernel/vfs/driver/lcd_panel.h) and the framebuffer console
 * (kernel/vfs/driver/fbcon.c).
 *
 * Lives next to its consumers under target/<t>/kernel/vfs/driver/.
 * lcd_panel.h resolves the right per-target file through the
 * target-specific include search path.
 */

#ifndef PPAP_TARGET_PICO1CALC_KERNEL_VFS_DRIVER_LCD_GEOM_H
#define PPAP_TARGET_PICO1CALC_KERNEL_VFS_DRIVER_LCD_GEOM_H

#define LCD_WIDTH 320
#define LCD_HEIGHT 320

#endif /* PPAP_TARGET_PICO1CALC_KERNEL_VFS_DRIVER_LCD_GEOM_H */
