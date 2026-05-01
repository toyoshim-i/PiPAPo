/*
 * proc_info.h — Process data types and externs for cross-module use
 *
 * Data-only header: pcb_t struct, state enum, proc_table/current externs.
 * No function declarations — those stay in kernel/core/proc/proc.h.
 *
 * VFS code that needs process context (tty, pipe, procfs, namei, fd)
 * includes this header instead of proc.h.
 */

#ifndef PPAP_KERNEL_COMMON_CORE_PROC_INFO_H
#define PPAP_KERNEL_COMMON_CORE_PROC_INFO_H

#include <stdint.h>

#include "common/ptrace.h"
#include "kernel/common/config.h"
#include "kernel/common/core/mem_layout.h"
#include "kernel/common/core/page_types.h"
#include "kernel/common/core/subsys_info.h"
#include "kernel/common/spinlock.h" /* core_id() for #define current */

/* Forward declaration — struct file is defined in fd/file.h.
 * We only store pointers here so the incomplete type is sufficient. */
struct file;

/* Signal handler type — matches signal/signal.h.
 * Duplicated here to avoid circular include (signal.h needs pcb_t). */
typedef void (*sighandler_t)(int);
#define NSIG 32

/* ── PCB_SP_OFFSET (must match switch.S) ─────────────────────────────── */

#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
#define PCB_SP_OFFSET 32u
#elif defined(__m68k__)
#define PCB_SP_OFFSET 44u
#define PCB_USP_OFFSET 48u
#elif defined(__riscv)
#define PCB_SP_OFFSET 48u
#elif defined(__xtensa__)
#define PCB_SP_OFFSET 0u
#elif defined(__ia16__)
#define PCB_SP_OFFSET 0u
#else
#error "Unsupported architecture — define PCB_SP_OFFSET"
#endif

#define TRACE_SW_BP_MAX 8 /* max software breakpoints per tracee  */
#define TRACE_HW_BP_MAX 4 /* max native hardware breakpoints       */

/* ── Types ───────────────────────────────────────────────────────────── */

/* pid_t: POSIX process ID type. */
typedef int32_t pid_t;

typedef enum {
  PROC_FREE = 0,        /* slot is not in use                              */
  PROC_RUNNABLE = 1,    /* ready to run, or currently executing            */
  PROC_SLEEPING = 2,    /* blocked until sleep_until SysTick count         */
  PROC_BLOCKED = 3,     /* blocked on vfork/waitpid                        */
  PROC_ZOMBIE = 4,      /* exited; slot freed when parent calls waitpid()  */
  PROC_TRACED_STOP = 5, /* stopped and waiting for tracer resume          */
} proc_state_t;

/*
 * user_page_ref_t — page-index + offset reference to user-space memory.
 */
typedef struct {
  page_id_t page;
  uint16_t off;
} user_page_ref_t;

static inline user_page_ref_t user_page_ref_invalid(void) {
  user_page_ref_t ref;
  ref.page = PAGE_ID_INVALID;
  ref.off = 0;
  return ref;
}

static inline user_page_ref_t user_to_page(page_id_t base, uint32_t user_off) {
  user_page_ref_t ref;
  ref.page = base + (page_id_t)(user_off / PAGE_SIZE);
  ref.off = (uint16_t)(user_off % PAGE_SIZE);
  return ref;
}

/* ── PCB struct ──────────────────────────────────────────────────────── */

typedef struct pcb {
  /*
   * Saved CPU context — architecture-dependent.
   * IMPORTANT: the byte offsets MUST match the #defines used in switch.S
   * (PCB_SP_OFFSET).  Do not reorder these fields.
   */
#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
  uint32_t r4, r5, r6, r7;   /* callee-saved low registers  (offsets 0-15)  */
  uint32_t r8, r9, r10, r11; /* callee-saved high registers (offsets 16-31) */
  uint32_t sp;               /* saved PSP                   (offset 32)     */
#elif defined(__m68k__)
  uint32_t d2, d3, d4, d5, d6, d7; /* callee-saved data regs  (offsets 0-23) */
  uint32_t a2, a3, a4, a5, a6; /* callee-saved addr regs  (offsets 24-43)  */
  uint32_t sp;                 /* saved SSP               (offset 44)      */
  uint32_t usp;                /* saved USP               (offset 48)      */
#elif defined(__riscv)
  uint32_t s0, s1;         /* callee-saved (offsets 0-7)               */
  uint32_t s2, s3, s4, s5; /* callee-saved (offsets 8-23)              */
  uint32_t s6, s7, s8, s9; /* callee-saved (offsets 24-39)             */
  uint32_t s10, s11;       /* callee-saved (offsets 40-47)             */
  uint32_t sp;             /* saved stack pointer     (offset 48)      */
  uint32_t kernel_sp;      /* kernel stack top for mscratch (offset 52) */
#elif defined(__xtensa__)
  uint32_t sp; /* saved stack pointer     (offset 0)       */
#elif defined(__ia16__)
  uint32_t sp;           /* saved kernel-stack SP (offset 0)          */
  uint16_t kernel_sp;    /* top of this process's 2 KB kernel stack   */
  uint16_t exec_user_ss; /* new user SS after execve (set by loader)  */
  uint16_t exec_user_sp; /* new user SP after execve (set by loader)  */
  /* Per-process shadow of the core↔VFS entry-stub globals (saved_cs,
   * saved_ip, vfs_saved_cs, vfs_saved_ip in core's .bss).  On every
   * context switch i16_sched_yield / i16_timer_isr copy the current
   * globals into the outgoing PCB's slots and reload them from the
   * incoming PCB's slots.  Stubs themselves still use the globals —
   * they're effectively per-process through this swap.  Offsets are
   * asserted in proc.c and must match the mov-insn displacements
   * used by switch.S. */
  uint16_t core_stub_saved_cs;
  uint16_t core_stub_saved_ip;
  uint16_t vfs_stub_saved_cs;
  uint16_t vfs_stub_saved_ip;
#else
#error "Unsupported architecture — define PCB register save area"
#endif

  /* ── Identity ───────────────────────────────────────────────────────── */
  pid_t pid;
  pid_t ppid;
  proc_state_t state;

  /* ── Memory ─────────────────────────────────────────────────────────── */
  page_id_t stack_page_id; /* page_id_t for 4 KB stack backing page */
  page_id_t user_pages[USER_PAGES_MAX]; /* page-backed user memory tracking */
  proc_image_t image; /* explicit process image layout / memory classes */
#if defined(__m68k__)
  void *user_stack_page; /* m68k: separate user stack page (USP target) */
#endif

  /* ── File descriptors ───────────────────────────────────────────────── */
  int16_t fd_map[FD_MAX]; /* per-process fd -> system descriptor ID map
                           * -1 (FD_DESC_NONE) = empty slot.
                           * VFS owns the file objects; core owns the map. */
  char cwd[64];           /* current working directory (Phase 2+)       */

  /* ── Scheduling ─────────────────────────────────────────────────────── */
  uint32_t ticks_remaining; /* SysTick ticks left in current time-slice   */
  uint32_t sleep_until;     /* wake when SysTick count reaches this value */
  int8_t running_on_core;   /* -1 = not running, 0/1 = core ID           */
  uint8_t is_idle;          /* 1 = idle thread (ticks count as idle)      */

  /* ── vfork / waitpid ──────────────────────────────────────────────── */
  struct pcb *vfork_parent; /* non-NULL while child shares parent's space */
#if defined(__ia16__)
  uint8_t vfork_frame_saved; /* 1 if 24B GP+IRET frame saved on kstack */
#endif
  int exit_status;    /* set by _exit(), read by waitpid()          */
  uintptr_t got_base; /* r9 value (GOT SRAM address) for PIC       */
  void *wait_channel; /* sleep/wakeup target (e.g. pipe_t*)        */

  /* ── Heap (brk) ──────────────────────────────────────────────────── */
  uintptr_t brk_base;    /* initial break = end of .data+.bss         */
  uintptr_t brk_current; /* current break (grows upward)             */

  /* ── Signals ─────────────────────────────────────────────────────── */
  sighandler_t sig_handlers[NSIG];  /* SIG_DFL(0) or SIG_IGN(1) or func */
  sighandler_t sig_restorers[NSIG]; /* sa_restorer per handler, used as the
                                       handler's return address by arches
                                       that need a user-space trampoline
                                       (ia16).  0 if unset. */
  uint32_t sig_pending;             /* bitmask of pending signals        */
  uint32_t sig_blocked;             /* bitmask of blocked signals        */

  /* ── Process identity / accounting (Phase 6 Step 14) ─────── */
  char comm[16];       /* command name (basename of exe)    */
  uint32_t utime;      /* user-mode ticks consumed          */
  uint32_t stime;      /* kernel-mode ticks consumed        */
  uint32_t start_time; /* boot tick when process created    */

  /* ── Process group / session (Phase 6 Step 7) ────────────────── */
  pid_t pgid;                      /* process group ID                  */
  pid_t sid;                       /* session ID                        */
  uint32_t umask_val;              /* file creation mask (default 022)  */
  user_page_ref_t clear_child_tid; /* set_tid_address reference         */

  /* ── m68k syscall restart (per-process, not global) ─────────── */
  uint8_t svc_needs_restart; /* set by blocking syscalls            */

  /* ── Tracing ─────────────────────────────────────────────────── */
  pid_t tracer_pid;             /* parent tracer PID, or 0 if none     */
  uint8_t trace_requested;      /* set by PTRACE_TRACEME until exec     */
  uint8_t trace_mode;           /* PPAP_TRACE_MODE_* bits              */
  uint8_t trace_surface;        /* PPAP_TRACE_SURFACE_* selection       */
  uint8_t trace_wait_pending;   /* waitpid(WSTOPPED) should report stop */
  uint8_t trace_syscall_phase;  /* 0=enter, 1=exit for SYSCALL mode     */
  uint8_t trace_subsys_phase;   /* 0=enter, 1=exit for subsystem mode   */
  uint8_t trace_step_pending;   /* pending ptrace single-step resume    */
  uint8_t trace_swbp_skip_once; /* skip one re-hit at same PC         */
  uintptr_t trace_swbp_skip_pc; /* PC to ignore once after sw-bp stop */
  struct {
    uintptr_t addr;
    uint8_t used;
    uint8_t enabled;
  } trace_swbp[TRACE_SW_BP_MAX];
  struct {
    uintptr_t addr;
    uint8_t used;
    uint8_t enabled;
  } trace_hwbp[TRACE_HW_BP_MAX];
  struct ppap_ptrace_event trace_event;

  /* ── Subsystem tag ───────────────────────────────────────────── */
  uint8_t subsys;    /* SUBSYS_PPAP, SUBSYS_HUMAN68K, etc.  */
  void *subsys_data; /* opaque per-process subsystem state  */

  /* ── CPU Operations (for emulated CPUs) ──────────────────────── */
  const struct cpu_ops *cpu_ops;
  void *cpu_state;

  /* ── Thread-local storage (TLS) ──────────────────────────── */
  uintptr_t tp_value; /* set/get_thread_area value          */

} pcb_t;

/* ── Globals ──────────────────────────────────────────────────────────── */

extern pcb_t proc_table[PROC_MAX];

extern pcb_t *current_core[2];

/* `current` — pointer to the PCB of the process executing on this core. */
#define current (current_core[core_id()])

extern volatile uint32_t *core_id_reg;

#endif /* PPAP_KERNEL_COMMON_CORE_PROC_INFO_H */
