/*
 * main.c — Unified kernel entry point
 *
 * Called from Reset_Handler (startup.S) after .data copy and .bss zero.
 * All target-specific init is delegated to target_early_init(),
 * target_late_init(), target_post_mount(), and target_init_path()
 * — see src/target/target.h.
 */

#include "fd/fd.h"
#include "fs/romfs.h"
#include "klog.h"
#include "mm/mem_region.h"
#include "mm/page.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "target/target.h"
#include "common/mod/mod_vfs.h"
#ifdef PPAP_HAS_BLKDEV
#include "blkdev/blkdev.h"
#include "blkdev/loopback.h"
#endif
#include "arch/arch.h"
#include "cpu/cpu.h"
#include "cpu/smp.h"
#include "common/errno.h"
#include "common/mod/mod_exec.h"
#include "common/spinlock.h"
#include "subsys/subsys.h"

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
      klogf("PANIC: mem_region_init failed (%d)\n", err);
      for (;;) arch_wfi();
    }
  }

  /* Process table init */
  proc_init();

  /* Register OS personality subsystem names with procfs */
  subsys_init();

  /* Log native + emulated CPU support */
  cpu_init();

  /* VFS layer + file pool for sys_open */
  mod_vfs.init();
  mod_vfs.file_pool_init();

#ifdef PPAP_HAS_BLKDEV
  /* Block device registry + loopback subsystem */
  blkdev_init();
  loopback_init();
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
      proc_table[0].stack_page_id = mm_ptr_to_page(stack_region.base);
    }
  }
#endif

  /* Bootstrap: mount root filesystem (needed to read /etc/fstab).
   * If an embedded romfs is present use it; otherwise delegate to the
   * target (e.g. x68k mounts a UFS ramdisk loaded by stage2). */
  if (&__romfs_start[0] != &__romfs_end[0]) {
    if (mod_vfs.mount("/", &romfs_ops, MNT_RDONLY, __romfs_start) == 0)
      klog("VFS: romfs mounted at /\n");
    else
      klog("VFS: romfs mount FAILED\n");
  } else if (target_mount_rootfs() == 0) {
    klog("VFS: rootfs mounted\n");
  } else {
    klog("VFS: rootfs mount FAILED\n");
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
      proc_table[0].stack_page_id = mm_ptr_to_page(stack_region.base);
    else
      proc_table[0].stack_page_id = PAGE_ID_INVALID;
  }
#endif
  if (proc_table[0].stack_page_id == PAGE_ID_INVALID) {
    klog("PANIC: no page for thread 0 stack\n");
    for (;;) arch_wfi();
  }

  /* Launch init as PID 1 (skipped when target_init_path() returns NULL,
   * e.g. m68k targets that don't have ELF user-mode binaries yet). */
  {
    const char *init_path = target_init_path();
    klog("INIT: starting\n");
    if (init_path) {
        pcb_t *init = proc_alloc();
      init->pgid = init->pid;
      init->sid = init->pid;
      mod_vfs.fd_stdio_init(init);

      int exec_err = mod_exec.do_execve(init, init_path, NULL);
      if (exec_err < 0) {
        klogf("INIT: %s failed, trying /bin/sh\n", init_path);
        exec_err = mod_exec.do_execve(init, "/bin/sh", NULL);
      }
      if (exec_err == 0) {
        init->state = PROC_RUNNABLE;
        klogf("INIT: pid=%u loaded\n", init->pid);
      } else {
        klogf("PANIC: no init or shell (err=%lu)\n", (unsigned long)(uint32_t)(-(int)exec_err));
        proc_free(init);
        for (;;) arch_wfi();
      }
    }
  }

  /* Launch Core 1 after init has PID 1 — core1_sched_entry() calls
   * proc_alloc() which would steal PID 1 if called earlier.
   * Self-stubs on QEMU (no SIO). */
  core1_launch(core1_sched_entry);

#ifdef __ia16__
  /* On i16, PIT timer must start after init is loaded — earlier timer
   * ISRs interfere with page allocation and exec setup. */
  {
    extern void timer_init(void);
    timer_init();
    klog("PIT: 100 Hz timer started\n");
  }
#endif
  klog("SCHED: starting scheduler\n");
  sched_start();
#ifdef __ia16__
  /* Direct jump to PID 1: load SP from PCB and restore via ISR path.
   * This avoids relying on the timer ISR for the first context switch. */
  {
    pcb_t *p1 = &proc_table[1];
    if (p1->state == PROC_RUNNABLE) {
      current_core[0] = p1;
      __asm__ volatile (
        "movw %0, %%sp\n\t"
        "popw %%es\n\t"
        "popw %%ds\n\t"
        "popw %%bp\n\t"
        "popw %%di\n\t"
        "popw %%si\n\t"
        "popw %%dx\n\t"
        "popw %%cx\n\t"
        "popw %%bx\n\t"
        "popw %%ax\n\t"
        "iret"
        : : "r"((uint16_t)p1->sp)
      );
    }
  }
#endif

  /* Run one immediate handoff so PID 1 starts without waiting for the
   * first timer preemption tick. */
  sched_yield();

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
        sched_yield();
      }
    }
#endif
    arch_wfi();
  }
}
