/*
 * bios_con.c -- BIOS INT 10h text output for IBM PC
 *
 * Uses BIOS teletype function (INT 10h, AH=0Eh) for screen output.
 * This works on all PC-compatible hardware without requiring direct
 * video RAM access.
 */

#include "bios_con.h"

void bios_putc(char c)
{
  __asm__ volatile (
    "int $0x10"
    :
    : "a"((unsigned short)(0x0E00u | (unsigned char)c)),
      "b"((unsigned short)0x0007u)  /* Page 0, light grey */
  );
}

void bios_puts(const char *s)
{
  while (*s)
    bios_putc(*s++);
}
