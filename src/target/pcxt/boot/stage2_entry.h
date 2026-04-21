/*
 * stage2_entry.h -- Symbols exported by stage2_entry.S
 *
 * boot_drive is written by _start2 from DL (the BIOS boot drive value
 * stage1 leaves there), then read throughout stage2 for every INT 13h
 * read.  print_char writes AL to both BIOS teletype (INT 10h AH=0Eh)
 * and COM1 and preserves all registers, so stage2.c calls it as a
 * near routine when it only has a `char` to emit.
 */

#ifndef PPAP_TARGET_PCXT_BOOT_STAGE2_ENTRY_H
#define PPAP_TARGET_PCXT_BOOT_STAGE2_ENTRY_H

#include <stdint.h>

extern uint8_t boot_drive;

void print_char(void);

#endif /* PPAP_TARGET_PCXT_BOOT_STAGE2_ENTRY_H */
