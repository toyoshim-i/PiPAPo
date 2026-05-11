/*
 * lcd_geom.h — M5Stack CardComputer LCD geometry
 *
 * Visible-window dimensions and panel-frame offsets for the ST7789V2
 * panel in the landscape orientation used by the CardComputer
 * (240 wide × 135 tall, USB-C on the right).  The ST7789V2 silicon
 * backs a 240×320 RAM frame; the visible 240×135 window sits at
 * column offset 40, row offset 53 in the controller's landscape
 * frame.  The SPI2 transport adds these offsets to every CASET /
 * RASET so callers (fbcon, panel driver, lcd_fill_rect) work in a
 * (0,0)-origin visible coordinate space.
 *
 * Consumed by the generic LCD driver (kernel/vfs/driver/lcd_panel.h)
 * and the framebuffer console (kernel/vfs/driver/fbcon.c).  Resolved
 * through the target's include search path.
 */

#ifndef PPAP_TARGET_XTENSA_CC_KERNEL_VFS_DRIVER_LCD_GEOM_H
#define PPAP_TARGET_XTENSA_CC_KERNEL_VFS_DRIVER_LCD_GEOM_H

#define LCD_WIDTH 240
#define LCD_HEIGHT 135

/* Panel-frame offsets — picked together with MADCTL=MV|MX (lcd_st7789.c).
 * Changing orientation requires updating both. */
#define LCD_COL_OFFSET 40u
#define LCD_ROW_OFFSET 53u

#endif /* PPAP_TARGET_XTENSA_CC_KERNEL_VFS_DRIVER_LCD_GEOM_H */
