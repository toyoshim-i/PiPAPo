/*
 * bios_con.h -- BIOS/VGA text console (output + keyboard input)
 */

#ifndef PPAP_TARGET_PCXT_KERNEL_VFS_DRIVER_BIOS_CON_H
#define PPAP_TARGET_PCXT_KERNEL_VFS_DRIVER_BIOS_CON_H

#include "kernel/vfs/tty.h"

void bios_putc(char c);
void bios_puts(const char *s);

extern const tty_backend_t bios_con_backend;

#endif /* PPAP_TARGET_PCXT_KERNEL_VFS_DRIVER_BIOS_CON_H */
