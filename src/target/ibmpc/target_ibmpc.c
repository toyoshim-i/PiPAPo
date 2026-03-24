/*
 * target_ibmpc.c — IBM PC target hooks for the PPAP kernel
 *
 * Phase P-3b: core kernel integration.  Provides target_early_init(),
 * target_late_init(), and other hooks that main.c calls during boot.
 */

#include "drivers/uart.h"
#include "drivers/bios_con.h"
#include "drivers/timer_pit.h"
#include "target/target.h"
#include "klog.h"

/* ── klog sink: serial + BIOS screen ─────────────────────────────────────── */

static int ibmpc_klog_putc(char c, void (*notify)(void))
{
  (void)notify;
  bios_putc(c);
  return 1;
}

/* ── Target hooks ────────────────────────────────────────────────────────── */

void target_early_init(void)
{
  uart_init();
  klog_set_mirror(ibmpc_klog_putc, (void (*)(void))0);
  klog("PiPAPo booting... [ibmpc]\n");
}

void target_late_init(void)
{
  timer_init();
  klog("PIT: 100 Hz timer started\n");
}

void target_post_mount(void)
{
  /* No tests or extra init for P-3b */
}

const char *target_init_path(void)
{
  /* No user-space init yet — kernel boots to idle */
  return (const char *)0;
}

const char *target_name(void)
{
  return "ibmpc";
}

uint32_t target_caps(void)
{
  return TARGET_CAP_REALUART;
}
