/*
 * proc.c — Process table and PCB lifecycle
 *
 * Manages a flat array of PROC_MAX process control blocks.  proc_table[0]
 * is permanently reserved for the initial kernel/init thread; slots 1..7
 * are available for user processes allocated via proc_alloc().
 *
 * proc_alloc() scans for a PROC_FREE slot — O(PROC_MAX) = O(8), negligible.
 * PIDs are assigned from a monotonically increasing counter so they are
 * unique for the lifetime of the kernel (no wraparound in Phase 1).
 */

#include "proc.h"

#include <stddef.h> /* NULL, offsetof */

#include "../klog.h"
#include "../mm/page.h"  /* PAGE_SIZE — for proc_setup_stack */
#include "../spinlock.h" /* SPIN_PROC */
#include "arch/ioregs.h"
#include "sched.h" /* sched_get_ticks — for start_time */

/* Default file creation mask (octal 022 → owner rw, group/other r) */
#define DEFAULT_UMASK 022

/* Verify PCB_SP_OFFSET matches the actual struct layout at compile time.
 * If this fires, update PCB_SP_OFFSET in proc.h to match offsetof(pcb_t,sp). */
_Static_assert(
    offsetof(pcb_t, sp) == PCB_SP_OFFSET,
    "PCB_SP_OFFSET does not match offsetof(pcb_t, sp) — update proc.h");

/* ── Globals ─────────────────────────────────────────────────────────────── */

pcb_t proc_table[PROC_MAX];
pcb_t *current_core[2] = {NULL, NULL};

/* Indirect core-ID register pointer for assembly (switch.S, svc.S).
 * Points to SIO_CPUID on RP2040, or core_id_zero on QEMU.
 * Assembly does: ldr rN, =core_id_reg; ldr rN, [rN]; ldr rN, [rN]
 * → two loads, no branches, works on both platforms. */
static uint32_t core_id_zero = 0;
volatile uint32_t *core_id_reg = &core_id_zero;

/* Monotonically increasing PID counter.  Starts at 1; pid 0 is the kernel. */
static pid_t next_pid = 1;

/* ── Public API ──────────────────────────────────────────────────────────── */

void proc_init(void) {
  /* Zero all slots and mark them free */
  for (uint32_t i = 0u; i < PROC_MAX; i++) {
    __builtin_memset(&proc_table[i], 0, sizeof(pcb_t));
    proc_table[i].state = PROC_FREE;
  }

  /* Pre-initialise slot 0 as the initial kernel thread.
   * stack_page is NULL: this thread runs on the initial kernel stack
   * set up by startup.S; no page_alloc() needed. */
  proc_table[0].pid = 0;
  proc_table[0].ppid = 0;
  proc_table[0].state = PROC_RUNNABLE;
  proc_table[0].ticks_remaining = PROC_DEFAULT_TICKS;
  proc_table[0].is_idle = 1;
  /* comm is set to "init" once do_execve() runs; "kernel" for now */
  __builtin_memcpy(proc_table[0].comm, "kernel", 7);

  current_core[0] = &proc_table[0];

  /* Point assembly's core_id_reg at the SIO_CPUID register on RP2040.
   * On QEMU (no SIO), it stays pointing at core_id_zero → always 0. */
  if (spin_have_hw()) core_id_reg = (volatile uint32_t *)0xD0000000u;

  /* ── Print boot diagnostic ─────────────────────────────────────────── */
  klogf(
      "PROC: process table  slots=%u  (pid 0 = kernel, pids 1-%u available)\n",
      (uint32_t)PROC_MAX, (uint32_t)(PROC_MAX - 1u));

#ifdef PPAP_TESTS
  /* ── Self-test ─────────────────────────────────────────────────────── */
  /* Allocate a slot, verify it looks sane, then free it. */
  pcb_t *p = proc_alloc();

  uint32_t ok = (p != NULL) && (p->pid == 1) &&
                (p->state == PROC_FREE) /* still FREE — caller sets state */
                && (current == &proc_table[0]) &&
                (current->state == PROC_RUNNABLE);

  if (p) proc_free(p);

  /* Reset PID counter so the first real process gets PID 1.
   * busybox init requires PID == 1. */
  next_pid = 1;

  klogf("PROC: self-test %s\n", ok ? "PASSED" : "FAILED");
#endif
}

pcb_t *proc_alloc(void) {
  uint32_t saved = spin_lock_irqsave(SPIN_PROC);
  pcb_t *result = NULL;

  /* Scan slots 1..PROC_MAX-1; slot 0 belongs to the kernel thread */
  for (uint32_t i = 1u; i < PROC_MAX; i++) {
    if (proc_table[i].state == PROC_FREE) {
      __builtin_memset(&proc_table[i], 0, sizeof(pcb_t));
      proc_table[i].pid = next_pid++;
      /* pgid and sid are left at 0 (from memset).
       * sys_vfork copies them from the parent (like real fork).
       * Only setsid/setpgid should change them explicitly. */
      proc_table[i].umask_val = DEFAULT_UMASK;
      proc_table[i].running_on_core = -1;
      proc_table[i].start_time = sched_get_ticks();
      /* state left as PROC_FREE — caller sets it to PROC_RUNNABLE
       * only after filling in stack_page and setting up the stack frame */
      result = &proc_table[i];
      break;
    }
  }

  spin_unlock_irqrestore(SPIN_PROC, saved);
  return result;
}

void proc_free(pcb_t *p) {
  if (!p) return;
  uint32_t saved = spin_lock_irqsave(SPIN_PROC);
  p->state = PROC_FREE;
  spin_unlock_irqrestore(SPIN_PROC, saved);
}

void proc_setup_stack(pcb_t *p, void (*entry)(void), uint32_t user_sp) {
  uint32_t *sp;
  if (user_sp)
    sp = (uint32_t *)(uintptr_t)user_sp;
  else
    sp = (uint32_t *)((uint8_t *)p->stack_page + PAGE_SIZE);

#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
  /*
   * ARM Cortex-M: two layers on the stack (high → low):
   *  1. Hardware exception frame (8 words): popped by EXC_RETURN.
   *  2. Software callee-saved frame: loaded by PendSV.
   *     - ARMv6-M: 8 words (r4–r11)
   *     - ARMv8-M: 9 words (r4–r11, EXC_RETURN) — EXC_RETURN encodes
   *       FPU frame type in bit 4 and is saved/restored per-process.
   */
  *--sp = XPSR_THUMB_BIT;        /* xpsr: Thumb bit (T=1)       */
  *--sp = (uint32_t)entry & ~1u; /* pc: entry point (bit0 clear)*/
  *--sp = EXC_RETURN_THREAD_PSP; /* lr: EXC_RETURN thread/PSP   */
  *--sp = 0u;                    /* r12                         */
  *--sp = 0u;                    /* r3                          */
  *--sp = 0u;                    /* r2                          */
  *--sp = 0u;                    /* r1                          */
  *--sp = 0u;                    /* r0                          */
  /* Software callee-saved frame (high → low) */
#if __ARM_ARCH >= 8
  /* EXC_RETURN: Thread mode, PSP, no FPU frame (bit 4 = 1).
   * New processes haven't used FPU yet, so no s16-s31 on stack. */
  *--sp = EXC_RETURN_THREAD_PSP; /* EXC_RETURN (bit 4 = 1)     */
#endif
  *--sp = 0u;           /* r11 */
  *--sp = 0u;           /* r10 */
  *--sp = 0u;           /* r9  */
  *--sp = 0u;           /* r8  */
  *--sp = 0u;           /* r7  */
  *--sp = 0u;           /* r6  */
  *--sp = 0u;           /* r5  */
  *--sp = 0u; /* r4  */ /* ← pcb_t.sp points here */

#elif defined(__m68k__)
  /*
   * M68K: exception frame built on kernel stack (stack_page / SSP).
   * SR = user mode so RTE switches to user mode + USP.
   * The caller must set p->usp to the desired user stack pointer.
   */
  /* Always build on kernel stack — ignore user_sp for m68k */
  sp = (uint32_t *)((uint8_t *)p->stack_page + PAGE_SIZE);
  *--sp = (uint32_t)entry;              /* PC: entry point (4 bytes)   */
  sp = (uint32_t *)((uint8_t *)sp - 2); /* back up 2 bytes for SR      */
  *(uint16_t *)sp = SR_USER;            /* SR: user mode, IPL=0        */
  /* Software register frame (a6..a0, d7..d0, high → low).
   * Must match movem.l %d0-%d7/%a0-%a6 register order. */
  *--sp = 0u;          /* a6 */
  *--sp = 0u;          /* a5 */
  *--sp = 0u;          /* a4 */
  *--sp = 0u;          /* a3 */
  *--sp = 0u;          /* a2 */
  *--sp = 0u;          /* a1 */
  *--sp = 0u;          /* a0 */
  *--sp = 0u;          /* d7 */
  *--sp = 0u;          /* d6 */
  *--sp = 0u;          /* d5 */
  *--sp = 0u;          /* d4 */
  *--sp = 0u;          /* d3 */
  *--sp = 0u;          /* d2 */
  *--sp = 0u;          /* d1 */
  *--sp = 0u; /* d0 */ /* ← pcb_t.sp points here */

#elif defined(__riscv)
  /*
   * RISC-V: build a trap frame matching trap.S layout (128 bytes).
   * When the scheduler switches to this process, trap.S restores all
   * registers from this frame and executes mret, which jumps to mepc.
   *
   * Trap frame layout (SP+0 → SP+124, low → high):
   *   x1(ra), x3(gp), x4(tp), x5-x7(t0-t2), x8-x9(s0-s1),
   *   x10-x17(a0-a7), x18-x27(s2-s11), x28-x31(t3-t6),
   *   mepc, mstatus
   *
   * mstatus.MIE must be set so interrupts are enabled after mret.
   * mstatus.MPIE is set by hardware on trap entry; we set it here so
   * mret re-enables interrupts.  MPP=M-mode (0b11 << 11).
   */
  sp -= 32; /* 32 words = 128 bytes = TRAP_FRAME_SIZE */
  /* Zero all general-purpose registers in the frame */
  for (int i = 0; i < 30; i++)
    sp[i] = 0u;
  /* Set gp (x3) at frame offset 1 */
  extern char __global_pointer$[];
  sp[1] = (uint32_t)(uintptr_t)__global_pointer$;
  /* mepc: entry point — mret will jump here */
  sp[30] = (uint32_t)(uintptr_t)entry;
  /* mstatus: MPP=M-mode, MPIE=1 (mret sets MIE from MPIE) */
  sp[31] = (3u << 11) | (1u << 7); /* MPP=M, MPIE=1 */

#elif defined(__xtensa__)
  /*
   * Xtensa windowed ABI: build a solicited frame matching switch.S.
   *
   * When xtensa_do_yield() switches to this process it:
   *   1. Loads PS and return PC from the solicited frame
   *   2. Executes RETW which triggers a window underflow
   *   3. Underflow handler reads the base save area below the frame
   *
   * Stack layout (high → low, 16-byte aligned):
   *   [top-16..top-4]   base save area (a0-a3 for retw underflow)
   *   [top-32..top-20]  solicited frame: exit, PC, PS, pad
   *
   * The entry point is encoded in the return PC slot.  For the first
   * switch into this process, RETW returns to the entry function.
   * We encode a0 with CALLINC=1 (bits 31:30 = 01) so RETW knows
   * the window increment was 4 (from a CALL4).
   */
  /* Align SP to 16 bytes (Xtensa ABI requirement) */
  sp = (uint32_t *)((uintptr_t)sp & ~0xFu);
  /* Base save area (16 bytes): retw underflow reads caller's a0-a3 */
  *--sp = 0u;                       /* a3 */
  *--sp = 0u;                       /* a2 */
  *--sp = 0u;                       /* a1 (unused for first entry) */
  *--sp = 0u;                       /* a0 (unused for first entry) */
  /* Solicited frame (16 bytes) — must match switch.S layout */
  *--sp = 0u;                       /* padding */
  *--sp = (1u << 18);               /* PS: WOE=1, INTLEVEL=0 */
  /* Return PC: encode with CALLINC=1 (bits 31:30 = 01) so RETW
   * decrements WindowBase by 1 (matching a CALL4 entry). */
  *--sp = ((uint32_t)entry & 0x3FFFFFFFu) | (1u << 30);  /* PC */
  *--sp = 0u;                       /* exit marker = 0 (solicited) */
#endif

  p->sp = (uint32_t)(uintptr_t)sp;
  p->ticks_remaining = PROC_DEFAULT_TICKS;
}
