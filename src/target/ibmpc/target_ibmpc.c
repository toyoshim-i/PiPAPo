/*
 * target_ibmpc.c -- IBM PC target hooks + P-1 standalone kmain
 *
 * Phase P-1: standalone kmain() that prints the boot banner and halts.
 * Does NOT link the full kernel (uint32_t pointer assumptions in shared
 * code need reconciliation for 16-bit real mode).
 *
 * Phase P-2+ will switch to the standard kmain() from kernel/main.c.
 */

#include "drivers/uart.h"
#include "drivers/bios_con.h"

/* -- Minimal klog for P-1 (serial + BIOS screen) ----------------------- */

static void klog(const char *msg)
{
  while (*msg) {
    uart_putc(*msg, 0);
    bios_putc(*msg);
    msg++;
  }
}

/* -- P-1 kmain: print banner and halt ---------------------------------- */

void kmain(void)
{
  uart_init();
  klog("PiPAPo booting... [ibmpc]\r\n");

  /* Halt -- P-2 will add timer, scheduler, and process support */
  for (;;)
    __asm__ volatile ("hlt");
}
