/*
 * target_qemu_m68k.c — Target implementation for QEMU virt m68k
 *
 * Implements the target hook API (target.h) for the QEMU virt m68k machine.
 * QEMU virt m68k: RAM at 0x0, Goldfish TTY at 0xFF008000, Goldfish RTC
 * timer at 0xFF006000.
 *
 * Run with:
 *   qemu-system-m68k -machine virt -cpu m68000 -nographic \
 *       -kernel ppap_qemu_m68k.elf
 */

#include <stddef.h>
#include <stdint.h>

#include "common/errno.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/arch.h"
#include "kernel/core/driver/timer_qemu_m68k.h"
#include "kernel/core/mm/page.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/proc/sched.h"
#include "kernel/core/signal/signal_check.h"
#include "kernel/core/syscall/syscall.h"
#include "target/target.h"

/* virt-ctrl device — built into QEMU m68k virt machine */
#define VIRT_CTRL_BASE 0xFF009000u
#define VIRT_CTRL_CMD 4u
#define VIRT_CTRL_CMD_HALT 2u

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
 * Maps the m68k register frame onto the shared syscall_dispatch() API:
 *   frame[0..3] = d1..d4 (args 0-3), nr = d0, a4 = d5, a5 = a0.
 * syscall_dispatch() writes the return value into frame[0] (d1 slot);
 * we then copy it to regs[0] (d0) for the caller.
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
  /* Boot banner printed from klog_init_logger() (VFS side) */
}

void target_late_init(void) {
  timer_init();
  mod_vfs.notify(VFS_EVENT_LATE_INIT);
}

void target_post_mount(void) {
  /* User-space init (/sbin/init) is launched by main.c via execve() */
}

const char *target_name(void) { return "qemu_m68k"; }

uint32_t target_caps(void) { return 0; /* No SD, no SPI, no Core 1 */ }

/*
 * QEMU poweroff via virt-ctrl device.
 *
 * Writing VIRT_CTRL_CMD_HALT to the CMD register triggers poweroff.
 */
void target_may_poweroff(uint8_t status) {
  (void)status;
  volatile uint32_t *cmd =
      (volatile uint32_t *)(VIRT_CTRL_BASE + VIRT_CTRL_CMD);
  *cmd = VIRT_CTRL_CMD_HALT;
  for (;;) __asm__ volatile("" ::: "memory");
}

/* Yield-test process — runs on its own stack, yields back to thread 0 */
void yield_test_process(void) {
  for (int i = 0; i < 4; i++) sched_switch();

  current->state = PROC_FREE;
  for (;;) sched_switch();
}
