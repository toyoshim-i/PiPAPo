/*
 * main.c — Unified kernel entry point
 *
 * Called from Reset_Handler (startup.S) after .data copy and .bss zero.
 * All target-specific init is delegated to target_early_init(),
 * target_late_init(), target_post_mount(), and target_init_path()
 * — see src/target/target.h.
 */

#include "kernel/core/mm/mem_region.h"
#include "kernel/core/mm/page.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/proc/sched.h"
#include "target/target.h"
#include "kernel/common/mod/mod_vfs.h"
#ifdef PPAP_HAS_BLKDEV
#include "kernel/core/driver/blkdev.h"
#include "kernel/core/driver/loopback.h"
#endif
#include "kernel/core/arch.h"
#include "kernel/core/cpu/cpu.h"
#include "kernel/core/cpu/smp.h"
#include "common/errno.h"
#include "kernel/core/exec/exec.h"
#include "kernel/common/spinlock.h"
#include "kernel/core/subsys/subsys.h"

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

  /* Memory manager + boot-time memory map */
  mm_init();
  {
    int err = mem_region_init();
    if (err < 0) {
      mod_vfs.klogf("PANIC: mem_region_init failed (%d)\n", err);
      for (;;) arch_wfi();
    }
  }

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

#ifdef PPAP_HAS_BLKDEV
  /* Block device registry + loopback subsystem */
  blkdev_init();
#if !defined(__ia16__)
  loopback_init();
#endif
#endif

  /* Target-specific late init: SD/ramblk, IRQ UART, MPU, Core 1 */
  target_late_init();

  /* Pre-mount test: allocate thread 0 stack now (before VFS mount
   * corrupts something on i16) */
#ifdef __ia16__
  {
    proc_image_segment_t stack_region;
    if (mem_region_alloc(&stack_region, PPAP_MEM_RAM_STACK, PAGE_SIZE,
                         PROC_IMAGE_SEG_OWNED | PROC_IMAGE_SEG_WRITABLE) == 0) {
      proc_table[0].stack_page_id = mem_region_ptr_to_page(stack_region.base);
    }
  }
#endif

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

  /* Parse /etc/fstab and mount all entries.
   * Skipped on i16 for now — UFS read returns corrupt data for
   * fstab (likely a 16-bit pointer/size issue in the VFS read path).
   * TODO: investigate and fix. */
#if !defined(__ia16__)
  mod_vfs.fstab_automount();
#endif

  /* Kernel integration tests (no-op unless PPAP_TESTS=ON) */
  target_post_mount();

  /* Give the kernel init thread (thread 0) its own PSP stack page.
   * On i16, this is done before mount (see above). */
#if !defined(__ia16__)
  {
    proc_image_segment_t stack_region;
    if (mem_region_alloc(&stack_region, PPAP_MEM_RAM_STACK, PAGE_SIZE,
                         PROC_IMAGE_SEG_OWNED | PROC_IMAGE_SEG_WRITABLE) == 0)
      proc_table[0].stack_page_id = mem_region_ptr_to_page(stack_region.base);
    else
      proc_table[0].stack_page_id = PAGE_ID_INVALID;
  }
#endif
  if (proc_table[0].stack_page_id == PAGE_ID_INVALID) {
    mod_vfs.klogf("PANIC: no page for thread 0 stack\n");
    for (;;) arch_wfi();
  }

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

      int exec_err = exec_execve(init, init_path, NULL);
      if (exec_err < 0) {
        mod_vfs.klogf("INIT: %s failed, trying /bin/sh\n", init_path);
        exec_err = exec_execve(init, "/bin/sh", NULL);
      }
      if (exec_err == 0) {
        init->state = PROC_RUNNABLE;
        mod_vfs.klogf("INIT: pid=%u loaded\n", init->pid);
      } else {
        mod_vfs.klogf("PANIC: no init or shell (err=%lu)\n", (unsigned long)(uint32_t)(-(int)exec_err));
        proc_free(init);
        for (;;) arch_wfi();
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
   * first timer preemption tick. */
  sched_switch();

  /* Idle thread — wake on every interrupt, flush LCD if needed, sleep. */
  for (;;) {
    sched_display_poll();
#if defined(__xtensa__)
    /* Semi-preemptive: timer ISR sets the flag, idle loop performs switch.
     * True preemptive switching (in interrupt return path) deferred to CC-4. */
    {
      extern volatile uint32_t xtensa_switch_pending;
      if (xtensa_switch_pending) {
        xtensa_switch_pending = 0;
        sched_switch();
      }
    }
#endif
    arch_wfi();
  }
}
