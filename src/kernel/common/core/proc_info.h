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
#include "kernel/common/core/page_types.h"
#include "kernel/common/core/proc_image.h"
#include "kernel/common/core/subsys_info.h"
#include "kernel/common/spinlock.h"

/* Forward declarations.  We only store pointers here so incomplete types are
 * sufficient. */
struct file;
struct kmutex;

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
#define PCB_KERNEL_SP_OFFSET 52u
#elif defined(__riscv)
#define PCB_SP_OFFSET 48u
#elif defined(__xtensa__)
#define PCB_SP_OFFSET 0u
#define PCB_KERNEL_SP_OFFSET 4u
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
  PROC_BLOCKED = 3,     /* blocked on a wait channel or process lifecycle  */
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

/*
 * PCB field protection rules:
 *
 * - context: saved-register fields are written by architecture switch/trap
 *   code for the running process.  Cross-process setup code writes them only
 *   before making a process runnable, or while the process is blocked in a
 *   lifecycle path such as vfork.
 * - identity/lifecycle: pid, ppid, state, wait_channel, sleep_until,
 *   running_on_core, vfork_parent, vfork_frame_saved, exit_status, and
 *   clear_child_tid are shared with scheduler, wait, exit, signal, or procfs
 *   paths.  Read or write them under SPIN_PROC unless the caller proves the
 *   target is current-only or not runnable yet.
 * - process-owned runtime state: fd_map, cwd, brk, signal masks/handlers,
 *   trace state, subsystem state, CPU state, TLS, comm, pgid/sid, umask, and
 *   memory-image/page-tracking fields are normally mutated by the owning
 *   process.  Cross-process readers must either hold SPIN_PROC for a stable
 *   lifecycle snapshot or tolerate best-effort procfs/debug output.
 * - accounting: utime/stime are written from timer context for the running
 *   process and sampled with scheduler/stat counters under SPIN_SCHED.
 * - kmutex_held: owned by kmutex.c and proc_free(); updates are serialized by
 *   the mutex implementation's SPIN_PROC critical sections.
 * - immutable setup fields: kernel_sp, is_idle, and fixed process-image
 *   layout are planted before the process can run and then treated as
 *   read-only.
 */

typedef struct pcb {
  /*
   * Saved CPU context — architecture-dependent.
   * IMPORTANT: the byte offsets MUST match the #defines used in switch.S
   * (PCB_SP_OFFSET).  Do not reorder these fields.
   */
#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
  /* These fields are arch-specific to ARM Cortex-M (MSP/PSP split).
   * Other archs must NOT reuse the names for different concepts — keep the
   * per-arch sections separate and add new fields rather than overloading.
   */
  uint32_t r4, r5, r6, r7;   /* callee-saved low registers  (offsets 0-15)  */
  uint32_t r8, r9, r10, r11; /* callee-saved high registers (offsets 16-31) */
  /* `sp` — saved MSP for this process, pointing at the base of the
   * unified 10-word SW frame {r4-r11, saved lr, saved PSP} that SVC
   * entry, PendSV, and arm_kernel_sched_switch all push.  See the
   * detailed comment block at the top of src/arch/arm_m/kernel/core/
   * switch.S for the full layout and restore protocol. */
  uint32_t sp; /* saved MSP                   (offset 32)     */
  /* `kernel_sp` — IMMUTABLE slot top of this process's MSP slot, planted
   * once by proc_kstack_init_slot() and never written by save paths.
   * arch_build_initial_frame uses it to compute where to write the
   * initial SW frame. */
  uint32_t kernel_sp; /* immutable slot top          (offset 36)     */
  uint32_t vfork_saved_frame[10]; /* parent's 32B HW frame + 8B {r7,lr}
                                     stub frame at vfork-trap; restored
                                     before the parent's next user-mode
                                     bx EXC_RETURN.  Slot 0 holds the
                                     patched r0 = child_pid. */
#elif defined(__m68k__)
  uint32_t d2, d3, d4, d5, d6, d7; /* callee-saved data regs  (offsets 0-23) */
  uint32_t a2, a3, a4, a5, a6; /* callee-saved addr regs  (offsets 24-43)  */
  uint32_t sp;                 /* saved SSP               (offset 44)      */
  uint32_t usp;                /* saved USP               (offset 48)      */
  uint32_t kernel_sp;          /* fixed kernel-stack top  (offset 52)      */
  uint32_t vfork_saved_ra;     /* parent's user-stack ra at vfork-trap;
                                  restored before parent returns to user   */
#elif defined(__riscv)
  uint32_t s0, s1;         /* callee-saved (offsets 0-7)               */
  uint32_t s2, s3, s4, s5; /* callee-saved (offsets 8-23)              */
  uint32_t s6, s7, s8, s9; /* callee-saved (offsets 24-39)             */
  uint32_t s10, s11;       /* callee-saved (offsets 40-47)             */
  uint32_t sp;             /* saved stack pointer     (offset 48)      */
  uint32_t kernel_sp;      /* kernel stack top for mscratch (offset 52) */
#elif defined(__xtensa__)
  uint32_t sp;                   /* saved solicited-frame SP (offset 0)      */
  uint32_t kernel_sp;            /* fixed kernel-stack top   (offset 4)      */
  uint32_t vfork_saved_frame_sp; /* low end of parent resume slice */
  uint32_t vfork_saved_frame_words; /* populated words in slice       */
  uint32_t vfork_saved_frame[128];  /* exception entry through frame   */
#elif defined(__ia16__)
  uint32_t sp;           /* saved kernel-stack SP (offset 0)          */
  uint16_t kernel_sp;    /* top of this process's kernel stack slot   */
  uint16_t exec_user_ss; /* new user SS after execve (set by loader)  */
  uint16_t exec_user_sp; /* new user SP after execve (set by loader)  */
  /* Per-process shadow of the core↔VFS entry-stub globals (saved_cs,
   * saved_ip, vfs_saved_cs, vfs_saved_ip in core's .bss).  On every
   * context switch i16_ctx_switch / i16_timer_isr copy the current
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

  /* ── Memory: owner-mutated; cross-process lifecycle under SPIN_PROC ─── */
  page_id_t stack_page_id; /* page_id_t for 4 KB stack backing page */
  page_id_t user_pages[USER_PAGES_MAX]; /* page-backed user memory tracking */
  proc_image_t image;    /* explicit process image layout / memory classes */
  void *user_stack_page; /* separate user stack page, when not stack_page_id */

  /* ── File descriptors: fd_map is process-owned; file objects are VFS ─── */
  int16_t fd_map[FD_MAX]; /* per-process fd -> system descriptor ID map
                           * -1 (FD_DESC_NONE) = empty slot.
                           * VFS owns the file objects; core owns the map. */
  char cwd[64];           /* current working directory (Phase 2+)       */

  /* ── Scheduling: SPIN_PROC except current time-slice bookkeeping ─────── */
  uint32_t ticks_remaining; /* SysTick ticks left in current time-slice   */
  uint32_t sleep_until;     /* wake when SysTick count reaches this value */
  int8_t running_on_core;   /* -1 = not running, 0/1 = core ID           */
  uint8_t is_idle;          /* 1 = idle thread (ticks count as idle)      */

  /* ── vfork / waitpid: lifecycle-shared, protect with SPIN_PROC ─────── */
  struct pcb *vfork_parent;   /* non-NULL while child shares parent's space */
  uint8_t vfork_frame_saved;  /* 1 if a parent-resume slice was saved during
                                 vfork and must be restored before this PCB
                                 returns to user mode.  The saved slice size
                                 and storage are arch-specific (see Phase 0
                                 in docs/proposals/no_stack_copy_on_vfork.md).
                               */
  int exit_status;            /* set by _exit(), read by waitpid()          */
  uintptr_t got_base;         /* r9 value (GOT SRAM address) for PIC       */
  void *wait_channel;         /* sleep/wakeup target (e.g. pipe_t*)        */
  struct kmutex *kmutex_held; /* process-owned sleepable mutex list        */

  /* ── Heap (brk): owning process mutates through sys_brk() ─────────── */
  uintptr_t brk_base;    /* initial break = end of .data+.bss         */
  uintptr_t brk_current; /* current break (grows upward)             */

  /* ── Signals: owner mutates masks/handlers; pending is SPIN_PROC ──── */
  sighandler_t sig_handlers[NSIG];  /* SIG_DFL(0) or SIG_IGN(1) or func */
  sighandler_t sig_restorers[NSIG]; /* sa_restorer per handler, used as the
                                       handler's return address by arches
                                       that need a user-space trampoline
                                       (ia16).  0 if unset. */
  uint32_t sig_pending;             /* bitmask of pending signals        */
  uint32_t sig_blocked;             /* bitmask of blocked signals        */

  /* ── Process identity / accounting ────────────────────────────────── */
  char comm[16];       /* command name (basename of exe)    */
  uint32_t utime;      /* user-mode ticks consumed          */
  uint32_t stime;      /* kernel-mode ticks consumed        */
  uint32_t start_time; /* boot tick when process created    */

  /* ── Process group / session: owner mutates; procfs snapshots best-effort */
  pid_t pgid;                      /* process group ID                  */
  pid_t sid;                       /* session ID                        */
  uint32_t umask_val;              /* file creation mask (default 022)  */
  user_page_ref_t clear_child_tid; /* set_tid_address reference         */

  /* ── Tracing: tracer/tracee lifecycle state, protect with SPIN_PROC ── */
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

  /* ── Subsystem tag: fixed after exec setup; subsystem data is owner state */
  uint8_t subsys;    /* SUBSYS_PPAP, SUBSYS_HUMAN68K, etc.  */
  void *subsys_data; /* opaque per-process subsystem state  */

  /* ── CPU Operations: fixed at exec/setup; CPU state is owner state ─── */
  const struct cpu_ops *cpu_ops;
  void *cpu_state;

  /* ── Thread-local storage (TLS): owning process mutates ────────────── */
  uintptr_t tp_value; /* set/get_thread_area value          */

} pcb_t;

/* ── Globals ──────────────────────────────────────────────────────────── */

extern pcb_t proc_table[PROC_MAX];

extern pcb_t *current_core[2];

/* `current` — pointer to the PCB of the process executing on this core. */
#define current (current_core[core_id()])

extern volatile uint32_t *core_id_reg;

#endif /* PPAP_KERNEL_COMMON_CORE_PROC_INFO_H */
