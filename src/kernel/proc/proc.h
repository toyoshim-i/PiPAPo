/*
 * proc.h — Process Control Block (PCB) definition and process table
 *
 * The PCB holds all per-process state: saved CPU registers, identity (pid/ppid),
 * memory (stack page, user pages), file descriptors, and scheduling fields.
 *
 * Layout (fits in 256 B):
 *   [0..35]   saved callee registers r4–r11 + sp  (must match switch.S offsets)
 *   [36..47]  pid, ppid, state
 *   [48..99]  stack_page + user_pages[USER_PAGES_MAX]
 *   fd_table[FD_MAX], cwd[64], scheduling fields, etc.
 *
 * PROC_MAX is intentionally small (8).  Every PCB lives in the static
 * proc_table[] array in kernel BSS — no dynamic allocation needed for Phase 1.
 */

#ifndef PPAP_PROC_PROC_H
#define PPAP_PROC_PROC_H

#include <stdint.h>
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
 * Layout: r4(0) r5(4) r6(8) r7(12) r8(16) r9(20) r10(24) r11(28) sp(32)
 */
#define PCB_SP_OFFSET  32u

/* ── Types ─────────────────────────────────────────────────────────────────── */

/* pid_t: POSIX process ID type.  Not provided by arm-none-eabi without
 * POSIX headers, so we define it here for bare-metal use. */
typedef int32_t pid_t;

typedef enum {
    PROC_FREE     = 0,   /* slot is not in use                              */
    PROC_RUNNABLE = 1,   /* ready to run, or currently executing            */
    PROC_SLEEPING = 2,   /* blocked until sleep_until SysTick count         */
    PROC_BLOCKED  = 3,   /* blocked on vfork/waitpid                        */
    PROC_ZOMBIE   = 4,   /* exited; slot freed when parent calls waitpid()  */
} proc_state_t;

typedef struct pcb {
    /*
     * Saved CPU context.
     * IMPORTANT: the byte offsets of r4–r11 and sp MUST match the
     * #defines used in switch.S.  Do not reorder these fields.
     */
    uint32_t r4, r5, r6, r7;       /* callee-saved low registers  (offsets 0–15)  */
    uint32_t r8, r9, r10, r11;     /* callee-saved high registers (offsets 16–31) */
    uint32_t sp;                    /* saved PSP                   (offset 32)     */
    /* r0–r3, r12, lr, pc, xpsr are saved automatically by hardware on exception
     * entry; the PendSV handler saves only the callee-saved registers above.   */

    /* ── Identity ───────────────────────────────────────────────────────── */
    pid_t        pid;
    pid_t        ppid;
    proc_state_t state;

    /* ── Memory ─────────────────────────────────────────────────────────── */
    void    *stack_page;        /* 4 KB page from page_alloc(): process stack */
    void    *user_pages[USER_PAGES_MAX]; /* user data pages (exec data segment) */

    /* ── File descriptors ───────────────────────────────────────────────── */
    struct file *fd_table[FD_MAX];
    char         cwd[64];       /* current working directory (Phase 2+)       */

    /* ── Scheduling ─────────────────────────────────────────────────────── */
    uint32_t ticks_remaining;   /* SysTick ticks left in current time-slice   */
    uint32_t sleep_until;       /* wake when SysTick count reaches this value */
    int8_t   running_on_core;   /* -1 = not running, 0/1 = core ID           */

    /* ── vfork / waitpid ──────────────────────────────────────────────── */
    struct pcb *vfork_parent;   /* non-NULL while child shares parent's space */
    int         exit_status;    /* set by _exit(), read by waitpid()          */
    uint32_t    got_base;       /* r9 value (GOT SRAM address) for PIC       */
    void       *wait_channel;   /* sleep/wakeup target (e.g. pipe_t*)        */

    /* ── Heap (brk) ──────────────────────────────────────────────────── */
    uint32_t    brk_base;      /* initial break = end of .data+.bss         */
    uint32_t    brk_current;   /* current break (grows upward)              */

    /* ── Signals ─────────────────────────────────────────────────────── */
    sighandler_t sig_handlers[NSIG]; /* SIG_DFL(0) or SIG_IGN(1) or func */
    uint32_t     sig_pending;        /* bitmask of pending signals        */
    uint32_t     sig_blocked;        /* bitmask of blocked signals        */

    /* ── Process identity / accounting (Phase 6 Step 14) ─────── */
    char         comm[16];           /* command name (basename of exe)    */
    uint32_t     utime;              /* user-mode ticks consumed          */
    uint32_t     stime;              /* kernel-mode ticks consumed        */
    uint32_t     start_time;         /* boot tick when process created    */

    /* ── Process group / session (Phase 6 Step 7) ────────────────── */
    pid_t        pgid;              /* process group ID                  */
    pid_t        sid;               /* session ID                        */
    uint32_t     umask_val;         /* file creation mask (default 022)  */
    int         *clear_child_tid;   /* set_tid_address pointer           */

    /* ── mmap regions (Phase 6 Step 7) ───────────────────────────── */
    struct {
        void    *addr;              /* base address of mapped region     */
        uint32_t pages;             /* number of pages in this region    */
    } mmap_regions[MMAP_REGIONS_MAX]; /* max concurrent mmap regions     */
} pcb_t;

/* ── Globals ────────────────────────────────────────────────────────────────── */

/* Flat process table: proc_table[0] is the initial kernel thread (pid 0).
 * All entries live in BSS and are zero-initialised by startup.S. */
extern pcb_t  proc_table[PROC_MAX];

/* Pointer to the currently executing PCB.  Always non-NULL after proc_init().
 * After Steps 8-9 convert assembly, this becomes:
 *   #define current (current_core[core_id()])
 * For now, kept as a real global for switch.S/svc.S literal pool references. */
extern pcb_t *current;

/* Per-core current process.  current_core[0] mirrors `current` until the
 * assembly handlers are converted to use core_id() indexing (Steps 8-9). */
extern pcb_t *current_core[2];

/* ── API ────────────────────────────────────────────────────────────────────── */

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
 * Set up an initial kernel stack frame for a new process so that
 * PendSV_Handler can restore it on the first context switch.
 *
 * Pre-condition: p->stack_page must already point to a 4 KB page obtained
 * from page_alloc().  After this call p->sp is set and the process is ready
 * to be made PROC_RUNNABLE.
 *
 * On entry to `entry`, all callee-saved registers are zero, r0–r3 are zero,
 * and lr = 0xFFFFFFFD (EXC_RETURN: Thread mode, PSP, basic frame).
 *
 * user_sp: the PSP value after the hardware frame pop.  For plain kernel
 * threads pass 0 (defaults to stack_page + PAGE_SIZE).  For exec'd
 * processes this points to the argc slot built by do_execve().
 */
void proc_setup_stack(pcb_t *p, void (*entry)(void), uint32_t user_sp);

#endif /* PPAP_PROC_PROC_H */
