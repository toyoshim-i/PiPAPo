/*
 * stage2_entry.h -- Symbols exported by stage2_entry.S
 *
 * boot_drive is written by _start2 from DL (the BIOS boot drive value
 * stage1 leaves there), then read throughout stage2 for every INT 13h
 * read.  print_char is not declared here: it takes its argument in AL
 * and is only ever reached via inline `asm("call print_char" ...)` in
 * stage2.c, so the linker resolves it directly from the asm reference.
 */

#ifndef PPAP_TARGET_PCXT_BOOT_STAGE2_ENTRY_H
#define PPAP_TARGET_PCXT_BOOT_STAGE2_ENTRY_H

#include <stdint.h>

extern uint8_t boot_drive;

#endif /* PPAP_TARGET_PCXT_BOOT_STAGE2_ENTRY_H */
