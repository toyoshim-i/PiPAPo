/*
 * xtensa_common.c — Shared Xtensa architecture state
 *
 * Provides:
 *   - Context switch pending flag (checked by timer ISR / yield)
 *   - CCOMPARE0 timer init and ISR
 *   - xtensa_do_switch() — C helper called from switch.S
 *   - Syscall dispatch (via ESP-IDF exception table)
 *   - Exception/fault handler (crash reporting)
 */

#include <stdint.h>
#include "cpu.h"
#include "../../kernel/proc/proc.h"
#include "../../kernel/proc/sched.h"
#include "../../kernel/syscall/syscall.h"
#include "../../kernel/klog.h"

/* ESP-IDF exception frame and handler registration API */
#include "xtensa_api.h"

/* Context switch pending flag.
 * Set by arch_yield() (via sched_tick or sched_yield).
 * Checked by switch.S / sched_yield to perform the switch.
 * Same pattern as riscv_switch_pending / m68k_switch_pending. */
volatile uint32_t xtensa_switch_pending = 0;

/* Timer ready flag — set by xtensa_timer_init().
 * arch_preempt_enable() checks this to avoid enabling the timer interrupt
 * before the ISR is registered. */
volatile uint32_t xtensa_timer_ready = 0;

/* Tick counter — incremented by timer ISR. */
volatile uint32_t xtensa_tick_count = 0;
volatile uint32_t xtensa_last_isr_epc1 = 0;

/* ── CCOMPARE0 timer ─────────────────────────────────────────────────────── */

/* Timer ISR — called from ESP-IDF's level-1 interrupt dispatch.
 * Rearms CCOMPARE0 and calls the shared scheduler tick handler. */
static void xtensa_timer_isr(void *arg)
{
    (void)arg;
    uint32_t cmp;
    __asm__ volatile("rsr %0, ccompare0" : "=a"(cmp));
    __asm__ volatile("wsr %0, ccompare0" :: "a"(cmp + XTENSA_TICK_INTERVAL));
    __asm__ volatile("esync");
    xtensa_tick_count++;
    sched_timer_tick(0);
}

void xtensa_timer_init(void)
{
    typedef void (*xt_handler)(void *);
    extern xt_handler xt_set_interrupt_handler(int n, xt_handler f, void *arg);

    /* Disable FreeRTOS's interrupt-level context switching.
     * _frxt_int_enter/_frxt_int_exit check this flag; when 0,
     * they skip TCB save/restore and ISR stack switch.
     * PPAP manages its own context switching. */
    {
        extern uint32_t port_xSchedulerRunning[];
        port_xSchedulerRunning[0] = 0;
    }

    /* Set CCOMPARE0 for first tick */
    uint32_t cc;
    __asm__ volatile("rsr %0, ccount" : "=a"(cc));
    __asm__ volatile("wsr %0, ccompare0" :: "a"(cc + XTENSA_TICK_INTERVAL));
    __asm__ volatile("esync");

    /* Register the CCOMPARE0 timer ISR */
    xt_set_interrupt_handler(XTENSA_TIMER0_INTNUM, xtensa_timer_isr, (void *)0);

    /* Set INTENABLE to ONLY the CCOMPARE0 bit (bit 6).
     * This replaces whatever FreeRTOS had configured (including bit 12
     * for SYSTIMER).  Do not OR — explicitly set to prevent stray
     * interrupts from firing. */
    __asm__ volatile("wsr %0, intenable" :: "a"(XTENSA_TIMER0_INTMASK));
    __asm__ volatile("rsync");

    xtensa_timer_ready = 1;
}

/* ── Context switch helper ───────────────────────────────────────────────── */

/* Called from switch.S (xtensa_do_yield) with the current SP.
 * Saves SP to old PCB, picks next process, returns new SP. */
uint32_t xtensa_do_switch(uint32_t current_sp)
{
    if (current)
        current->sp = current_sp;
    pcb_t *next = sched_next();
    current_core[core_id()] = next;
    return next->sp;
}

/* ── Syscall handler ──────────────────────────────────────────────────────── */

/* Called by ESP-IDF's exception dispatch when EXCCAUSE == SYSCALL (1).
 * The XtExcFrame has all registers saved; we extract syscall args from
 * a2-a7 and call the shared syscall_dispatch(). */
static void xtensa_syscall_handler(XtExcFrame *frame)
{
    /* Advance PC past the 3-byte SYSCALL instruction */
    frame->pc += 3;

    /* syscall_dispatch(frame, nr, a4, a5):
     *   frame[0..3] = a2,a3,a4,a5 (contiguous in XtExcFrame)
     *   nr          = a7 (syscall number)
     *   a4, a5      = 5th/6th args */
    syscall_dispatch((uint32_t *)&frame->a2, (uint32_t)frame->a7,
                     (uint32_t)frame->a4, (uint32_t)frame->a5);

    /* exec_pending: execve built a new-process frame at current->sp.
     * Reload the frame's PC/PS/SP so rfe jumps to the new program. */
    if (exec_pending[0]) {
        exec_pending[0] = 0;
        uint32_t *nf = (uint32_t *)(uintptr_t)current->sp;
        /* new-process frame: [0]=exit=1, [1]=entry, [2]=PS, [3]=user_sp */
        frame->pc = nf[1];
        frame->ps = nf[2];
        frame->a1 = nf[3];
        frame->a2 = 0;
    }

    /* svc_restart: blocking syscall needs re-execution */
    if (svc_restart[0]) {
        svc_restart[0] = 0;
        frame->pc -= 3;  /* rewind to SYSCALL instruction */
        frame->a2 = svc_saved_a0[0];
    }

    /* Context switch via cooperative yield.
     *
     * Xtensa has no PendSV — we can't modify the exception frame to switch
     * to a process with a solicited (windowed-ABI) context.  Instead, call
     * sched_yield() which invokes xtensa_do_yield() to perform a proper
     * save/restore through the windowed call chain.
     *
     * When sched_yield() returns, we're back in THIS handler, which then
     * returns to ESP-IDF's dispatcher → rfe → user code.
     *
     * Yield if:
     *  - preemption pending (timer slice expired)
     *  - current process blocked (e.g. read() with no data)
     */
    /* Context switch via cooperative yield. */
    if (xtensa_switch_pending ||
        (current && current->state != PROC_RUNNABLE && !current->is_idle)) {
        xtensa_switch_pending = 0;
        sched_yield();
    }
}

/* ── Fault handler ───────────────────────────────────────────────────────── */

static const char *exccause_name(uint32_t cause)
{
    static const char *names[] = {
        [0]  = "IllegalInsn",   [1]  = "Syscall",
        [2]  = "InsnFetchErr",  [3]  = "LoadStoreErr",
        [4]  = "Level1Int",     [5]  = "Alloca",
        [6]  = "IntDivZero",    [8]  = "Privileged",
        [9]  = "LoadAlign",     [12] = "InsnPIF_Data",
        [13] = "InsnPIF_Addr",  [14] = "LoadPIF_Data",
        [15] = "LoadPIF_Addr",  [16] = "StorePIF_Data",
        [17] = "StorePIF_Addr", [20] = "InsnTLBMiss",
        [24] = "LoadTLBMiss",   [28] = "StoreTLBMiss",
        [29] = "CoprocessorN",
    };
    if (cause < sizeof(names) / sizeof(names[0]) && names[cause])
        return names[cause];
    return "Unknown";
}

/* Called for all unhandled exceptions (illegal insn, load/store error, etc.).
 * Prints crash info for debugging, kills user processes, halts on kernel
 * faults. */
static void xtensa_fault_handler(XtExcFrame *frame)
{
    uint32_t cause = (uint32_t)frame->exccause;
    klogf("EXCEPTION: %s (cause=%u) pc=%x vaddr=%x\n",
          exccause_name(cause), cause,
          (uint32_t)frame->pc, (uint32_t)frame->excvaddr);
    klogf("  a0=%x a1=%x a2=%x a3=%x a4=%x a5=%x\n",
          (uint32_t)frame->a0, (uint32_t)frame->a1,
          (uint32_t)frame->a2, (uint32_t)frame->a3,
          (uint32_t)frame->a4, (uint32_t)frame->a5);
    klogf("  a6=%x a7=%x a8=%x a9=%x a10=%x a11=%x\n",
          (uint32_t)frame->a6, (uint32_t)frame->a7,
          (uint32_t)frame->a8, (uint32_t)frame->a9,
          (uint32_t)frame->a10, (uint32_t)frame->a11);

    if (current && !current->is_idle) {
        klogf("  pid=%u comm=%s — killed\n", current->pid, current->comm);
        current->state = PROC_ZOMBIE;
        current->exit_status = 128 + 11; /* SIGSEGV */
        /* Must actually switch — arch_yield() only sets a flag, which
         * wouldn't be checked before rfe returns to the faulting instr. */
        sched_yield();
        return;
    }

    /* Kernel fault: halt */
    klog("PANIC: kernel exception — halting\n");
    for (;;)
        __asm__ volatile ("waiti 15");
}

/* ── ESP-IDF abort/assert override ──────────────────────────────────────── */

/* Override ESP-IDF's abort() and __assert_func() so kernel-level panics
 * (heap corruption, assertion failures, etc.) go through our handler
 * with a clear message instead of triggering an IllegalInstruction via
 * ESP-IDF's panic_abort(). */

/* Linked via -Wl,--wrap=abort and -Wl,--wrap=__assert_func
 * (set in CMakeLists.txt).  The __wrap_ prefix replaces the original. */

void __wrap_abort(void)
{
    klog("ABORT called");
    if (current && !current->is_idle) {
        klogf(" (pid=%u comm=%s)\n", current->pid, current->comm);
        current->state = PROC_ZOMBIE;
        current->exit_status = 128 + 6; /* SIGABRT */
        arch_yield();
        /* Try to let scheduler run — if we return, hang. */
    } else {
        klog(" — kernel abort, halting\n");
    }
    for (;;)
        __asm__ volatile ("waiti 15");
}

void __wrap___assert_func(const char *file, int line, const char *func,
                          const char *expr)
{
    klogf("ASSERT FAILED: %s:%u", file ? file : "?", (uint32_t)line);
    if (func) klogf(" (%s)", func);
    if (expr) klogf(": %s", expr);
    klog("\n");
    __wrap_abort();
}

/* ── Trap initialization ─────────────────────────────────────────────────── */

void xtensa_trap_init(void)
{
    /* Install SYSCALL handler */
    xt_set_exception_handler(EXCCAUSE_SYSCALL, xtensa_syscall_handler);

    /* Install fault handler for all other exception causes */
    for (int i = 0; i < 30; i++) {
        if (i == (int)EXCCAUSE_SYSCALL ||
            i == (int)EXCCAUSE_LEVEL1_INT ||
            i == (int)EXCCAUSE_ALLOCA)
            continue;  /* handled by ESP-IDF / windowed ABI */
        xt_set_exception_handler(i, xtensa_fault_handler);
    }
}

/* ── Initial stack frame for new processes ──────────────────────────────── */

/* Build a "new-process" frame for switch.S (exit marker = 1).
 *
 * Layout (20 bytes, 16-byte aligned):
 *   [SP+0 ] = 1       (exit marker — triggers direct-jump path in switch.S)
 *   [SP+4 ] = entry   (entry address, NOT windowed-encoded)
 *   [SP+8 ] = PS      (UM=1, WOE=0, INTLEVEL=0)
 *   [SP+12] = user_sp (stack pointer passed to user code in a1)
 *   [SP+16] = a0      (return address; needed by vfork child, 0 for exec)
 *
 * switch.S detects exit != 0, loads entry/PS/user_sp/a0, and does jx.
 * This avoids the base-save-area overlap that would corrupt a1 with the PC.
 *
 * The 'sp' parameter points to the argc/argv area on the user stack.
 * We build the frame below it and store the original sp as user_sp.
 */
uint32_t *arch_build_initial_frame(uint32_t *sp, void (*entry)(void))
{
    uint32_t user_sp = (uint32_t)(uintptr_t)sp;
    sp = (uint32_t *)((uintptr_t)sp & ~0xFu);  /* 16-byte align */
    user_sp = (uint32_t)(uintptr_t)sp;          /* use aligned value */

    *--sp = 0;                                   /* [SP+16] a0 = 0 (unused by _start) */
    *--sp = user_sp;                            /* [SP+12] user SP */
    /* PS for user process: WOE=0 (call0 ABI, no window operations),
     * UM=1 (user mode — routes exceptions through UserExceptionVector
     *       which properly dispatches SYSCALL, interrupts, etc.
     *       KernelExceptionVector panics on ALL exceptions.),
     * INTLEVEL=0 (interrupts enabled for preemption). */
    *--sp = (1u << 5);                          /* [SP+8]  PS: UM=1 */
    *--sp = (uint32_t)(uintptr_t)entry;         /* [SP+4]  entry addr */
    *--sp = 1u;                                 /* [SP+0]  exit = 1 (new proc) */
    return sp;
}
