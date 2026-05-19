/*
 * target_x68k.c — Target implementation for X68000
 *
 * Console: TVRAM via IOCS (TRAP #15).
 * Timer:   MFP (MC68901) Timer-C, 100 Hz.
 * Storage: 5-inch floppy, mounted live via iocs_blk ("fd0").
 *
 * Boot chain: stage1 (sector 0) → stage2 (sectors 1-3) → kernel at
 * 0x006000.  Stage2 reads the on-floppy 44bsd UFS, loads /boot/kernel,
 * copies .vectors to 0x000000 (restoring the IPL ROM handlers for
 * TRAP #15, autovectors 24-31, MFP 64-79 and DMAC 80-127) and jumps
 * to Reset_Handler at 0x006400.
 */

#include <stdint.h>

#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/boot.h"
#include "kernel/core/driver/timer_x68k.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/proc/sched.h"
#include "kernel/core/signal/signal_check.h"
#include "kernel/core/syscall/syscall.h"
#ifdef PPAP_HAS_BLKDEV
#include "kernel/vfs/driver/iocs_blk.h"
#endif
#include "target/target.h"

/* ── TRAP #0 syscall dispatch ────────────────────────────────────────── *
 *
 * Called from m68k_trap0_handler (trap.S) with a pointer to the saved
 * register frame.  Register layout (Linux m68k syscall convention):
 *
 *   regs[0]  = d0  (syscall number → overwritten with return value)
 *   regs[1]  = d1  (arg 1)
 *   regs[2]  = d2  (arg 2)
 *   regs[3]  = d3  (arg 3)
 *   regs[4]  = d4  (arg 4)
 *   regs[5]  = d5  (arg 5)
 *   regs[8]  = a0  (arg 6)
 *
 * Maps the m68k register frame onto the shared syscall_dispatch() API.
 * ────────────────────────────────────────────────────────────────────── */

void m68k_syscall_entry(uint32_t *regs) {
  uint32_t nr = regs[0]; /* d0 = syscall number */
  uint32_t a4 = regs[5]; /* d5 = arg 5 */
  uint32_t a5 = regs[8]; /* a0 = arg 6 */

  syscall_dispatch(&regs[1], nr, a4, a5);

  /* Move the return value from frame[0] (d1 slot) to d0 (Linux m68k ABI). */
  regs[0] = regs[1];

  signal_check(regs);
}

/* ── Target hooks ────────────────────────────────────────────────────── */

void target_early_init(void) {
  /* Patch hardware interrupt vectors BEFORE the first IOCS call.
   *
   * Stage2 copies the kernel's .vectors to 0x000000 before jumping here,
   * so autovectors 25-30 (OPM, MFP, FDC, VSYNC, DMA …) and MFP vectored
   * interrupts 64-79 all point to Default_Handler (stop #0x2700) at this
   * point.  Some IOCS functions (e.g. _B_CLR_ST) temporarily lower the CPU
   * interrupt priority level to allow VSYNC-sync; if an interrupt fires
   * before the vectors are overridden the CPU would halt inside the IOCS.
   *
   * NOTE: timer_init() is called later in target_late_init() and installs
   * m68k_timer_isr at vt[69].  It must come AFTER this loop; the loop
   * filling vt[64-79] with m68k_irq_ignore runs here so that call is safe.
   */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
  volatile uint32_t *vt = (volatile uint32_t *)0x0;
  uint32_t ignore = (uint32_t)(uintptr_t)m68k_irq_ignore;

  /* Stage2 preserved ALL IPL ROM autovectors (24-31) and MFP vectors
   * (64-79) so that IOCS functions work correctly.  Here we only need
   * to replace remaining Default_Handler entries (vectors 10-23, 34-46,
   * 80-255) with m68k_irq_ignore to prevent CPU halts on unexpected
   * interrupts.
   *
   * Vectors already correctly set:
   *   0-9:   SSP, Reset, Bus/Address error, etc. (kernel handlers)
   *   24-31: Autovectors (IPL ROM — VSYNC, SCC, etc.)
   *   32:    TRAP #0 — syscall
   *   33:    TRAP #1 — yield
   *   47:    TRAP #15 — IOCS dispatch (IPL ROM)
   *   64-79: MFP vectored interrupts (IPL ROM — keyboard, timers)
   *
   * timer_init() overwrites vector 69 (Timer-C) with m68k_timer_isr
   * for preemptive scheduling.
   */
  for (uint32_t v = 10u; v < 24u; v++) vt[v] = ignore;
  for (uint32_t v = 34u; v < 47u; v++) /* TRAP #2-#14 */
    vt[v] = ignore;
  for (uint32_t v = 48u; v < 64u; v++) /* TRAP #16+ and reserved */
    vt[v] = ignore;
  /* Vectors 80-127 hold HD63450 DMAC channel NIV/EIV entries that stage2
   * preserved from the IPL ROM.  Skipping them keeps IOCS _B_READ /
   * _B_WRITE able to ack their DMA completion IRQs.  Bare-rte ignore is
   * fine for everything above 127. */
  for (uint32_t v = 128u; v < 256u; v++) vt[v] = ignore;
#pragma GCC diagnostic pop

  /* Diagnostic: "Po" — kernel reached (TRAP #15 = IPL IOCS, restored by
   * stage2). Together with stage2's "PiPA" this completes the "PiPAPo" banner.
   */
  {
    register uint32_t d0 asm("d0") = 0x20u;
    register uint32_t d1 asm("d1") = 'P';
    asm volatile("trap #15" : "+r"(d0) : "r"(d1) : "a0", "a1", "memory");
    d0 = 0x20u;
    d1 = (uint32_t)'o';
    asm volatile("trap #15" : "+r"(d0) : "r"(d1) : "a0", "a1", "memory");
  }

  /* Register the klog logger now so the klogf calls below produce output;
   * vfs_init() will call klog_init_logger() again later, idempotently. */
  mod_vfs.notify(VFS_EVENT_MODULE_READY);

  mod_vfs.klogf(" booting... [x68k]\n");
  mod_vfs.klogf("Console: X68000 IOCS (TVRAM)\n");
}

void target_late_init(void) {
  /* MFP Timer-C for preemptive scheduling */
  timer_init();
  /* TTY backends, input polls, secondary logger — all VFS side */
  mod_vfs.notify(VFS_EVENT_LATE_INIT);
#ifdef PPAP_HAS_BLKDEV
  iocs_blk_init();
#endif
}

int target_mount_rootfs(void) {
#ifdef PPAP_HAS_BLKDEV
  /* iocs_blk registered "fd0" against the IOCS-backed floppy in
   * target_late_init.  Mount it as the rootfs — same 44bsd UFS that
   * stage2 loaded /boot/kernel from. */
  return mod_vfs.mount_ufs("/", 0, "fd0");
#else
  return -1;
#endif
}

void target_post_mount(void) {
  /* Nothing target-specific needed after rootfs mount */
}

const char *target_init_path(void) {
#ifdef PPAP_TESTS
#ifdef PPAP_TESTS_EXTENDED
  return "/bin/runtests_ext";
#else
  return "/bin/runtests";
#endif
#else
  return "/sbin/init";
#endif
}

const char *target_name(void) { return "x68k"; }

uint32_t target_caps(void) {
  return 0; /* Phase X-3: no SD, no SPI, no Core 1 */
}

/* Yield-test process — runs on its own stack, yields back to thread 0 */
void yield_test_process(void) {
  for (int i = 0; i < 4; i++) sched_switch();

  current->state = PROC_FREE;
  for (;;) sched_switch();
}
