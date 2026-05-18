/*
 * main.c — Unified kernel entry point
 *
 * Called from Reset_Handler (startup.S) after .data copy and .bss zero.
 * All target-specific init is delegated to target_early_init(),
 * target_late_init(), target_post_mount(), and target_init_path()
 * — see src/target/target.h.
 */

#include "common/errno.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/common/spinlock.h"
#include "kernel/core/arch.h"
#include "kernel/core/cpu/cpu.h"
#include "kernel/core/cpu/smp.h"
#include "kernel/core/exec/exec.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/mm/page.h"
#include "kernel/core/panic.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/proc/sched.h"
#include "kernel/core/subsys/subsys.h"
#include "target/target.h"

/* Linker-provided romfs image location in flash */
extern const uint8_t __romfs_start[];
extern const uint8_t __romfs_end[];

/* ── Kernel entry point ──────────────────────────────────────────────────── */

void kmain(void) {
  /* Release any stale hardware spinlocks left over from a previous
   * session (e.g. GDB reload).  Must happen before any spinlock use. */
  spin_locks_reset();

  /* Target-specific early init: UART console, clock PLL, SPI bus */
  target_early_init();

  /* Init ordering: mem_region_init runs first so it can reserve any
   * architecture-specific text/rodata arenas; mm_init's page-pool
   * grab can then cross-check its range against those arenas where
   * the arch demands it.  On targets where mem_region_init is a
   * no-op (most arches), the ordering is irrelevant. */
  {
    int err = mem_region_init();
    if (err < 0) panic("mem_region_init failed (%d)\n", err);
  }

  /* Memory manager + boot-time memory map */
  mm_init();

  /* Process table init */
  proc_init();

  /* Init fd_map for all processes (BSS is zero, but FD_DESC_NONE is -1) */
  for (int _p = 0; _p < PROC_MAX; _p++)
    for (int _f = 0; _f < FD_MAX; _f++)
      proc_table[_p].fd_map[_f] = FD_DESC_NONE;

  /* Register OS personality subsystem names with procfs */
  subsys_init();

  /* Log native + emulated CPU support */
  cpu_init();

  /* VFS layer + file pool for sys_open */
  mod_vfs.init();
  mod_vfs.fd_pool_init();

  /* Target-specific late init: SD/ramblk, IRQ UART, MPU, Core 1 */
  target_late_init();

  /* Bootstrap: mount root filesystem (needed to read /etc/fstab).
   * If an embedded romfs is present use it; otherwise delegate to the
   * target (e.g. x68k mounts a UFS ramdisk loaded by stage2). */
  if (&__romfs_start[0] != &__romfs_end[0]) {
    if (mod_vfs.mount_romfs("/", MNT_RDONLY, __romfs_start) == 0)
      mod_vfs.klogf("VFS: romfs mounted at /\n");
    else
      mod_vfs.klogf("VFS: romfs mount FAILED\n");
  } else if (target_mount_rootfs() == 0) {
    mod_vfs.klogf("VFS: rootfs mounted\n");
  } else {
    mod_vfs.klogf("VFS: rootfs mount FAILED\n");
  }

  /* Parse /etc/fstab and mount all entries. */
  mod_vfs.fstab_automount();

  /* Kernel integration tests (no-op unless PPAP_TESTS=ON) */
  target_post_mount();

  /* Give the kernel init thread (thread 0) its own page-backed stack when
   * the architecture needs one.  ia16 and RISC-V use fixed kernel-stack slots
   * instead, so no extra page-backed allocation is needed there. */
#if !defined(__ia16__) && !defined(__riscv)
  {
    region_t r;
    if (mem_region_alloc(PPAP_MEM_RAM_STACK, PAGE_SIZE, 0, &r) == 0)
      proc_table[0].stack_page_id = r.base_page;
    else
      proc_table[0].stack_page_id = PAGE_ID_INVALID;
  }
#endif
#if defined(__ia16__) || defined(__riscv)
  proc_table[0].stack_page_id = PAGE_ID_INVALID;
#endif
#if !defined(__ia16__) && !defined(__riscv)
  if (proc_table[0].stack_page_id == PAGE_ID_INVALID)
    panic("no page for thread 0 stack\n");
#endif

  /* Launch init as PID 1 (skipped when target_init_path() returns NULL,
   * e.g. m68k targets that don't have ELF user-mode binaries yet). */
  {
    const char *init_path = target_init_path();
    mod_vfs.klogf("INIT: starting\n");
    if (init_path) {
      pcb_t *init = proc_alloc();
      init->pgid = init->pid;
      init->sid = init->pid;
      mod_vfs.fd_stdio_init(init);

      /* init starts with empty env; children will inherit whatever init
       * (and any shell it spawns) chooses to export.  E-2 onward will
       * let the shell's env flow to execve'd children. */
      int exec_err = exec_execve_simple(init, init_path);
      if (exec_err < 0) {
        mod_vfs.klogf("INIT: %s failed, trying /bin/sh\n", init_path);
        exec_err = exec_execve_simple(init, "/bin/sh");
      }
      if (exec_err == 0) {
        init->state = PROC_RUNNABLE;
        mod_vfs.klogf("INIT: pid=%u loaded\n", init->pid);
      } else {
        proc_free(init);
        panic("no init or shell (err=%lu)\n",
              (unsigned long)(uint32_t)(-(int)exec_err));
      }
    }
  }

  /* Start deferred timer now that all floppy-backed exec is done.
   * Must happen before sched_start() so the first context switch can
   * fire.  No-op on targets that already started the timer. */
  target_enable_deferred_timer();

  /* Launch Core 1 after init has PID 1 — core1_sched_entry() calls
   * proc_alloc() which would steal PID 1 if called earlier.
   * Self-stubs on QEMU (no SIO). */
  core1_launch(core1_sched_entry);

  mod_vfs.klogf("SCHED: starting scheduler\n");
  sched_start();

  /* Run one immediate handoff so PID 1 starts without waiting for the
   * first timer preemption tick.  This is a bootstrap handoff, not a
   * suspended process continuation. */
  sched_switch();

  /* Idle thread — wake on every interrupt, flush LCD if needed, sleep.
   *
   * sched_idle_poll() returns non-zero if any of the polled work raised
   * an arch_yield() (e.g. tty_rx_notify woke a blocked reader on a
   * polled-input target).  Honor it immediately from the idle path so the
   * wakeup does not have to wait for the next timer tick.  On Cortex-M
   * PendSV self-pends via NVIC and arch_yield_consume() always returns 0. */
  for (;;) {
    if (sched_idle_poll()) sched_switch();
    arch_wfi();
  }
}
