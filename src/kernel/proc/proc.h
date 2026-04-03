/*
 * proc.h — Process Control Block (PCB) definition and process table
 *
 * The PCB holds all per-process state: saved CPU registers, identity
 * (pid/ppid), memory (stack page, user pages), file descriptors, and scheduling
 * fields.
 *
 * Layout:
 *   - saved CPU context (must match switch.S offsets)
 *   - identity / scheduling state
 *   - memory ownership pointers and process-image metadata
 *   - fd table, cwd, signals, tracing, subsystem, mmap state
 *
 * PROC_MAX is intentionally small (8).  Every PCB lives in the static
 * proc_table[] array in kernel BSS — no dynamic allocation needed for Phase 1.
 */

#ifndef PPAP_KERNEL_PROC_PROC_H
#define PPAP_KERNEL_PROC_PROC_H

#include <stdint.h>

#include "arch/arch.h"
#include "../common/spinlock.h" /* core_id() — needed by #define current */
#include "../mm/mem_layout.h"
#include "../mm/page.h"        /* page_id_t, PAGE_ID_INVALID */
#include "common/ptrace.h"
#include "config.h"

/* Forward declaration — struct file is defined in fd/file.h (Step 10).
 * We only store pointers here so the incomplete type is sufficient. */
struct file;

/* Signal handler type — matches signal/signal.h.
 * Duplicated here to avoid circular include (signal.h needs pcb_t). */
typedef void (*sighandler_t)(int);
#define NSIG 32

/*
 * PCB_SP_OFFSET: byte offset of the `sp` field within pcb_t.
 * Used in switch.S to save/restore the process stack pointer.
 * A _Static_assert in proc.c verifies this at compile time.
 *
 * ARM Cortex-M: r4..r11 (8×4=32) → sp at offset 32
 * m68k:         d2..d7,a2..a6 (11×4=44) → sp at offset 44
 * RISC-V:       s0..s11 (12×4=48) → sp at offset 48
 * Xtensa:       sp only (windowed ABI, all state on stack) → sp at offset 0
 */
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

/* Subsystem tags — identifies which OS personality a process uses.
 * The F-line handler checks this to dispatch Human68k DOS calls
 * vs. crashing non-Human68k processes that hit F-line opcodes. */
#define SUBSYS_PPAP 0     /* native PPAP ELF binary (default)    */
#define SUBSYS_HUMAN68K 1 /* Human68k X-format binary             */
#define SUBSYS_CPM 2      /* CP/M 2.2 .COM binary (Z80 emulated) */
#define SUBSYS_SOS 3      /* S-OS SWORD binary (Z80 emulated)    */
#define TRACE_SW_BP_MAX 8 /* max software breakpoints per tracee  */
#define TRACE_HW_BP_MAX 4 /* max native hardware breakpoints       */

/* ── Types ───────────────────────────────────────────────────────────────────
 */

/*
 * user_page_ref_t — page-index + offset reference to user-space memory.
 *
 * Syscall handlers resolve user pointer arguments to this type via
 * user_to_page(), then access user memory through mem_region_page_read
 * / mem_region_page_write.  This works on all architectures:
 *
 *   i16:  raw register value is DS-relative offset = process-relative
 *   flat: page = mem_region_ptr_to_page(raw), off = raw & (PAGE_SIZE - 1)
 *   MMU:  future page-table walk
 *
 * user_to_page() takes the process's base page_id_t and a byte offset
 * from the start of the process's contiguous page allocation.  The
 * caller extracts the offset from the raw syscall argument:
 *
 *   i16:    user_off = (uint32_t)raw_arg
 *   32-bit: user_off = (uint32_t)raw_arg
 *                     - mem_region_page_linear(proc_page_backed_base(p))
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

static inline user_page_ref_t user_to_page(page_id_t base,
                                           uint32_t user_off) {
  user_page_ref_t ref;
  ref.page = base + (page_id_t)(user_off / PAGE_SIZE);
  ref.off = (uint16_t)(user_off % PAGE_SIZE);
  return ref;
}

/* pid_t: POSIX process ID type.  Not provided by arm-none-eabi without
 * POSIX headers, so we define it here for bare-metal use. */
typedef int32_t pid_t;

typedef enum {
  PROC_FREE = 0,        /* slot is not in use                              */
  PROC_RUNNABLE = 1,    /* ready to run, or currently executing            */
  PROC_SLEEPING = 2,    /* blocked until sleep_until SysTick count         */
  PROC_BLOCKED = 3,     /* blocked on vfork/waitpid                        */
  PROC_ZOMBIE = 4,      /* exited; slot freed when parent calls waitpid()  */
  PROC_TRACED_STOP = 5, /* stopped and waiting for tracer resume          */
} proc_state_t;

typedef struct pcb {
  /*
   * Saved CPU context — architecture-dependent.
   * IMPORTANT: the byte offsets MUST match the #defines used in switch.S
   * (PCB_SP_OFFSET).  Do not reorder these fields.
   *
   * ARM Cortex-M: callee-saved r4–r11 + saved PSP.
   *   r0–r3, r12, lr, pc, xpsr are saved by hardware on exception entry.
   * m68k (future): callee-saved d2–d7/a2–a6 + saved SP.
   */
#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
  uint32_t r4, r5, r6, r7;   /* callee-saved low registers  (offsets 0–15)  */
  uint32_t r8, r9, r10, r11; /* callee-saved high registers (offsets 16–31) */
  uint32_t sp;               /* saved PSP                   (offset 32)     */
#elif defined(__m68k__)
  uint32_t d2, d3, d4, d5, d6, d7; /* callee-saved data regs  (offsets 0–23) */
  uint32_t a2, a3, a4, a5, a6; /* callee-saved addr regs  (offsets 24–43)  */
  uint32_t sp;                 /* saved SSP               (offset 44)      */
  uint32_t usp;                /* saved USP               (offset 48)      */
#elif defined(__riscv)
  uint32_t s0, s1;             /* callee-saved (offsets 0–7)               */
  uint32_t s2, s3, s4, s5;    /* callee-saved (offsets 8–23)              */
  uint32_t s6, s7, s8, s9;    /* callee-saved (offsets 24–39)             */
  uint32_t s10, s11;           /* callee-saved (offsets 40–47)             */
  uint32_t sp;                 /* saved stack pointer     (offset 48)      */
  uint32_t kernel_sp;          /* kernel stack top for mscratch (offset 52) */
#elif defined(__xtensa__)
  uint32_t sp;                 /* saved stack pointer     (offset 0)       */
#elif defined(__ia16__)
  uint32_t sp;                 /* saved kernel-stack SP (offset 0)          */
  uint16_t kernel_stack_top;   /* top of this process's 2 KB kernel stack   */
  uint16_t exec_user_ss;       /* new user SS after execve (set by loader)  */
  uint16_t exec_user_sp;       /* new user SP after execve (set by loader)  */
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
  int16_t fd_map[FD_MAX]; /* per-process fd → system descriptor ID map
                           * -1 (FD_DESC_NONE) = empty slot.
                           * VFS owns the file objects; core owns the map. */
  char cwd[64]; /* current working directory (Phase 2+)       */

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
  int exit_status;          /* set by _exit(), read by waitpid()          */
  uintptr_t got_base;       /* r9 value (GOT SRAM address) for PIC       */
  void *wait_channel;       /* sleep/wakeup target (e.g. pipe_t*)        */

  /* ── Heap (brk) ──────────────────────────────────────────────────── */
  uintptr_t brk_base;   /* initial break = end of .data+.bss         */
  uintptr_t brk_current; /* current break (grows upward)             */

  /* ── Signals ─────────────────────────────────────────────────────── */
  sighandler_t sig_handlers[NSIG]; /* SIG_DFL(0) or SIG_IGN(1) or func */
  uint32_t sig_pending;            /* bitmask of pending signals        */
  uint32_t sig_blocked;            /* bitmask of blocked signals        */

  /* ── Process identity / accounting (Phase 6 Step 14) ─────── */
  char comm[16];       /* command name (basename of exe)    */
  uint32_t utime;      /* user-mode ticks consumed          */
  uint32_t stime;      /* kernel-mode ticks consumed        */
  uint32_t start_time; /* boot tick when process created    */

  /* ── Process group / session (Phase 6 Step 7) ────────────────── */
  pid_t pgid;           /* process group ID                  */
  pid_t sid;            /* session ID                        */
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

  /* ── Thread-local storage (TLS) ──────────────────────────── */
  uintptr_t tp_value; /* set/get_thread_area value          */

} pcb_t;

/* ── Globals ──────────────────────────────────────────────────────────────────
 */

/* Flat process table: proc_table[0] is the initial kernel thread (pid 0).
 * All entries live in BSS and are zero-initialised by startup.S. */
extern pcb_t proc_table[PROC_MAX];

/* Per-core current process pointer.  Indexed by core_id() (0 or 1).
 * Assembly (switch.S, svc.S) uses core_id_reg indirection to index this. */
extern pcb_t *current_core[2];

/* `current` — pointer to the PCB of the process executing on this core.
 * Expands to current_core[core_id()]: single MMIO read on RP2040, zero on QEMU.
 * Works as both lvalue and rvalue: `current->pid` and `current = p` both work.
 */
#define current (current_core[core_id()])

/* Indirect pointer to core-ID register, used by assembly (switch.S, svc.S).
 * Points to SIO_CPUID (0xD0000000) on RP2040, or a zero variable on QEMU.
 * Assembly dereferences twice: &core_id_reg → pointer → core_id value. */
extern volatile uint32_t *core_id_reg;

/* ── API ──────────────────────────────────────────────────────────────────────
 */

/*
 * Initialise the process table.
 * - Clears all slots and marks them PROC_FREE.
 * - Pre-initialises proc_table[0] as the initial kernel thread (pid 0,
 *   PROC_RUNNABLE) and sets current = &proc_table[0].
 * - Runs a brief self-test and prints the result over UART.
 * Must be called once from kmain() after UART and mm are ready.
 */
void proc_init(void);

/*
 * Allocate a free PCB slot (slots 1..PROC_MAX-1).
 * Clears the slot, assigns a unique pid, and returns a pointer to it.
 * Returns NULL if all slots are in use.
 * The caller is responsible for setting state, stack_page, and any other
 * fields before making the process PROC_RUNNABLE.
 */
pcb_t *proc_alloc(void);

/*
 * Release a PCB slot.
 * Marks the PCB PROC_FREE; the caller must have already freed any pages
 * held by the process (stack_page, user_pages[]) before calling this.
 * No-op if p is NULL.
 */
void proc_free(pcb_t *p);

/*
 * Track page-backed user memory in user_pages[].
 * Returns the number of tracked pages, or -ENOMEM if the range would
 * exceed USER_PAGES_MAX.
 */
int proc_track_page_range(pcb_t *p, uint32_t start_slot,
                          page_id_t base_page_id, uint32_t n_pages);

/*
 * Track one page-backed user page in user_pages[].
 * Returns 0 on success, or -ENOMEM if the slot is out of range.
 */
int proc_track_page(pcb_t *p, uint32_t slot, page_id_t page_id);

/* Return the first tracked page-backed page ID, or PAGE_ID_INVALID. */
page_id_t proc_page_backed_base(const pcb_t *p);

/* Resolve a raw user pointer to a page+offset reference for process p. */
static inline int proc_user_ptr_to_page_ref(const pcb_t *p, uintptr_t user_ptr,
                                            user_page_ref_t *ref) {
  page_id_t base_page;

  if (!p || !ref) return -1;
  base_page = proc_page_backed_base(p);
  if (base_page == PAGE_ID_INVALID) {
    *ref = user_page_ref_invalid();
    return -1;
  }
  ref->page = arch_user_ptr_to_page(base_page, user_ptr, &ref->off);
  if (ref->page == PAGE_ID_INVALID) {
    *ref = user_page_ref_invalid();
    return -1;
  }
  return 0;
}

/* Return the last tracked page-backed page ID, or PAGE_ID_INVALID. */
page_id_t proc_last_page_backed_base(const pcb_t *p);

/* Count contiguous tracked page-backed pages from the first tracked slot. */
uint32_t proc_page_backed_count(const pcb_t *p);

/* Return the slot index of the first tracked page, or USER_PAGES_MAX. */
uint32_t proc_first_page_backed_slot(const pcb_t *p);

/* Count all tracked page-backed pages regardless of slot layout. */
uint32_t proc_tracked_page_count(const pcb_t *p);

/* Return nonzero if addr is inside any tracked page-backed user page. */
int proc_page_backed_contains(const pcb_t *p, uintptr_t addr);

/* Clear page-backed tracking slots without freeing underlying pages. */
void proc_clear_page_tracking(pcb_t *p);

/* Copy tracked page slots from one process to another. */
void proc_copy_page_tracking(pcb_t *dst, const pcb_t *src);

/* Save tracked page slots from process into an array snapshot. */
void proc_copy_page_tracking_to_array(const pcb_t *src,
                                      page_id_t dst[USER_PAGES_MAX]);

/* Restore tracked page slots for process from an array snapshot. */
void proc_restore_page_tracking_from_array(pcb_t *dst,
                                           const page_id_t src[USER_PAGES_MAX]);

/* Free tracked pages in [start_slot, end_slot) and clear the slots. */
void proc_release_tracked_pages(pcb_t *p, uint32_t start_slot,
                                uint32_t end_slot);

/* Free tracked pages in p that are not shared with shared_owner. */
void proc_release_private_tracked_pages(pcb_t *p, const pcb_t *shared_owner);

/* Free all tracked pages recorded in an array snapshot. */
void proc_release_tracked_pages_from_array(page_id_t pages[USER_PAGES_MAX]);

/* Free pages in snapshot array that are not shared with shared[] slots. */
void proc_release_private_tracked_pages_from_array(
  page_id_t pages[USER_PAGES_MAX],
  const page_id_t shared[USER_PAGES_MAX]);

/*
 * Set up an initial kernel stack frame for a new process so that
 * PendSV_Handler can restore it on the first context switch.
 *
 * Pre-condition: p->stack_page_id must already reference a 4 KB stack backing
 * page. After this call p->sp is set and the process is ready to be made
 * PROC_RUNNABLE.
 *
 * On entry to `entry`, all callee-saved registers are zero, r0–r3 are zero,
 * and lr = 0xFFFFFFFD (EXC_RETURN: Thread mode, PSP, basic frame).
 *
 * user_sp: the PSP value after the hardware frame pop.  For plain kernel
 * threads pass 0 (defaults to stack_page + PAGE_SIZE).  For exec'd
 * processes this points to the argc slot built by execve().
 */
void proc_setup_stack(pcb_t *p, void (*entry)(void), uintptr_t user_sp);

#endif /* PPAP_KERNEL_PROC_PROC_H */
