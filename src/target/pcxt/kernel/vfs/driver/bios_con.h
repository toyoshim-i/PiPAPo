/*
 * bios_con.h -- BIOS INT 10h text output
 */

#ifndef PPAP_TARGET_PCXT_KERNEL_VFS_DRIVER_BIOS_CON_H
#define PPAP_TARGET_PCXT_KERNEL_VFS_DRIVER_BIOS_CON_H

void bios_putc(char c);
void bios_puts(const char *s);

#endif /* PPAP_TARGET_PCXT_KERNEL_VFS_DRIVER_BIOS_CON_H */
