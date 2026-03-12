/*
 * sys_proc.c — Process-related syscall implementations
 *
 *   sys_exit(status)    — terminate the calling process
 *   sys_getpid()        — return the calling process's PID
 *   sys_vfork(frame)    — create child process (parent blocked)
 *   sys_waitpid(pid,st) — wait for child to exit, reap zombie
 *   sys_execve(path,argv) — replace process image with new ELF binary
 */

#include "syscall.h"
#include "../proc/proc.h"
#include "../proc/sched.h"
#include "../exec/exec.h"
#include "../fd/fd.h"
#include "../mm/page.h"
#include "../errno.h"
#include "../signal/signal.h"
#include "../klog.h"
#include "../ecpu/ecpu_z80.h"
#include "../ecpu/ecpu_m68k.h"
#include "../subsys/ppap_m68k_bridge.h"
#include "../../target/target.h"
#include "arch/arch.h"
#include "common/ptrace.h"
#include "common/wait.h"
#include <string.h>

/* Wait status encoding (POSIX-compatible) */
#define W_EXITCODE(ret) (((ret) & 0xff) << 8)
#define W_STOPCODE(sig) ((((sig) & 0xff) << 8) | 0x7f)

#define TRACE_PHASE_ENTER  0
#define TRACE_PHASE_EXIT   1
#define TRACE_MODE_MASK \
    (PPAP_TRACE_MODE_PPAP_SYSCALL | PPAP_TRACE_MODE_SUBSYS_CALL)

static void trace_clear_swbp(pcb_t *target);

static pcb_t *trace_find_tracee(pid_t tracer_pid, long pid)
{
    if (pid <= 0)
        return NULL;
    for (uint32_t i = 1; i < PROC_MAX; i++) {
        pcb_t *p = &proc_table[i];
        if (p->state == PROC_FREE || p->pid != (pid_t)pid)
            continue;
        if (p->tracer_pid != tracer_pid)
            continue;
        return p;
    }
    return NULL;
}

static pcb_t *trace_find_process(long pid)
{
    if (pid <= 0)
        return NULL;
    for (uint32_t i = 1; i < PROC_MAX; i++) {
        pcb_t *p = &proc_table[i];
        if (p->state == PROC_FREE || p->pid != (pid_t)pid)
            continue;
        return p;
    }
    return NULL;
}

static void trace_wake_tracer(const pcb_t *tracee)
{
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        pcb_t *p = &proc_table[i];
        if (p->state == PROC_FREE || p->pid != tracee->tracer_pid)
            continue;
        if (p->state == PROC_BLOCKED)
            p->state = PROC_RUNNABLE;
        break;
    }
}

static void trace_fill_event(uint32_t event, uint32_t abi, uint32_t nr,
                             uint32_t a0, uint32_t a1, uint32_t a2,
                             uint32_t a3, uint32_t a4, uint32_t a5,
                             int32_t ret, uint32_t flags)
{
    current->trace_event.event = event;
    current->trace_event.flags = flags;
    current->trace_event.abi = abi;
    current->trace_event.nr = nr;
    current->trace_event.args[0] = a0;
    current->trace_event.args[1] = a1;
    current->trace_event.args[2] = a2;
    current->trace_event.args[3] = a3;
    current->trace_event.args[4] = a4;
    current->trace_event.args[5] = a5;
    current->trace_event.ret = ret;
}

static void trace_stop_current(int restart)
{
    current->trace_wait_pending = 1;
    current->state = PROC_TRACED_STOP;
    trace_wake_tracer(current);
#if defined(__m68k__)
    /* Avoid nested TRAP #1 switching from inside syscall/exception paths
     * for non-restart trace stops (exec stop, syscall-exit, subsys stops).
     * Defer the switch to the trap return path instead. */
    if (!restart) {
        arch_yield();
        return;
    }
#endif
    if (restart)
        set_svc_restart();
    sched_yield();
}

static void trace_reset_mode_state(pcb_t *p, uint8_t mode)
{
    p->trace_mode = mode;
    if (!(mode & PPAP_TRACE_MODE_PPAP_SYSCALL))
        p->trace_syscall_phase = TRACE_PHASE_ENTER;
    if (!(mode & PPAP_TRACE_MODE_SUBSYS_CALL))
        p->trace_subsys_phase = TRACE_PHASE_ENTER;
}

static void trace_resume_target(pcb_t *target)
{
    target->trace_wait_pending = 0;
    if (target->state == PROC_TRACED_STOP)
        target->state = PROC_RUNNABLE;
    sched_yield();
}

static void trace_regs_init(struct ppap_ptrace_regs *regs,
                            uint32_t regset, uint32_t abi, uint32_t words)
{
    __builtin_memset(regs, 0, sizeof(*regs));
    regs->regset = regset;
    regs->abi = abi;
    regs->words = words;
}

static int trace_fill_arm_regs(const pcb_t *target, struct ppap_ptrace_regs *regs)
{
    const uint32_t *sp = (const uint32_t *)(uintptr_t)target->sp;

    trace_regs_init(regs, PPAP_TRACE_REGSET_ARM, target->trace_event.abi, 17);
    regs->regs[0] = sp[8];
    regs->regs[1] = sp[9];
    regs->regs[2] = sp[10];
    regs->regs[3] = sp[11];
    regs->regs[4] = sp[0];
    regs->regs[5] = sp[1];
    regs->regs[6] = sp[2];
    regs->regs[7] = sp[3];
    regs->regs[8] = sp[4];
    regs->regs[9] = sp[5];
    regs->regs[10] = sp[6];
    regs->regs[11] = sp[7];
    regs->regs[12] = sp[12];
    regs->regs[13] = (uint32_t)(uintptr_t)(sp + 16);
    regs->regs[14] = sp[13];
    regs->regs[15] = sp[14];
    regs->regs[16] = sp[15];
    return 0;
}

#if defined(__m68k__)
static int trace_fill_m68k_frame_regs(const pcb_t *target,
                                      struct ppap_ptrace_regs *regs)
{
    const uint32_t *frame = (const uint32_t *)(uintptr_t)target->sp;
    const uint8_t *exc = (const uint8_t *)(uintptr_t)target->sp + 60;

    trace_regs_init(regs, PPAP_TRACE_REGSET_M68K, target->trace_event.abi, 20);
    for (uint32_t i = 0; i < 15; i++)
        regs->regs[i] = frame[i];
    regs->regs[15] = target->usp;
    regs->regs[16] = ((uint32_t)*(const uint16_t *)(const void *)(exc + 2) << 16)
                   | *(const uint16_t *)(const void *)(exc + 4);
    regs->regs[17] = *(const uint16_t *)(const void *)exc;
    regs->regs[18] = target->usp;
    regs->regs[19] = target->sp;
    return 0;
}
#endif

static int trace_fill_m68k_emu_regs(const pcb_t *target,
                                    struct ppap_ptrace_regs *regs)
{
    const ppap_m68k_exec_state_t *state =
        (const ppap_m68k_exec_state_t *)target->subsys_data;
    const m68k_state_t *cpu;

    if (!state)
        return -EINVAL;
    cpu = &state->m68k;

    trace_regs_init(regs, PPAP_TRACE_REGSET_M68K, target->trace_event.abi, 20);
    for (uint32_t i = 0; i < 8; i++) {
        regs->regs[i] = cpu->d[i];
        regs->regs[8 + i] = cpu->a[i];
    }
    regs->regs[16] = cpu->pc;
    regs->regs[17] = cpu->sr;
    regs->regs[18] = cpu->usp;
    regs->regs[19] = cpu->ssp;
    return 0;
}

static int trace_fill_z80_regs(const pcb_t *target, struct ppap_ptrace_regs *regs)
{
    const z80_state_t *cpu = (const z80_state_t *)target->subsys_data;

    if (!cpu)
        return -EINVAL;

    trace_regs_init(regs, PPAP_TRACE_REGSET_Z80, target->trace_event.abi, 18);
    regs->regs[0] = z80_af(cpu);
    regs->regs[1] = z80_bc(cpu);
    regs->regs[2] = z80_de(cpu);
    regs->regs[3] = z80_hl(cpu);
    regs->regs[4] = cpu->ix;
    regs->regs[5] = cpu->iy;
    regs->regs[6] = cpu->sp;
    regs->regs[7] = cpu->pc;
    regs->regs[8] = ((uint16_t)cpu->a2 << 8) | cpu->f2;
    regs->regs[9] = ((uint16_t)cpu->b2 << 8) | cpu->c2;
    regs->regs[10] = ((uint16_t)cpu->d2 << 8) | cpu->e2;
    regs->regs[11] = ((uint16_t)cpu->h2 << 8) | cpu->l2;
    regs->regs[12] = cpu->iff1;
    regs->regs[13] = cpu->iff2;
    regs->regs[14] = cpu->im;
    regs->regs[15] = cpu->wz;
    regs->regs[16] = cpu->i;
    regs->regs[17] = cpu->r;
    return 0;
}

static uint32_t trace_surface_mask_for(const pcb_t *target)
{
    uint32_t mask = PPAP_PTRACE_SURFACE_MASK_REAL;

    if (target->subsys == SUBSYS_CPM)
        mask |= PPAP_PTRACE_SURFACE_MASK_ECPU;
    if (target->subsys == SUBSYS_PPAP && target->subsys_data)
        mask |= PPAP_PTRACE_SURFACE_MASK_ECPU;
    return mask;
}

static uint32_t trace_default_surface_for(const pcb_t *target)
{
    if (trace_surface_mask_for(target) & PPAP_PTRACE_SURFACE_MASK_ECPU)
        return PPAP_TRACE_SURFACE_ECPU;
    return PPAP_TRACE_SURFACE_REAL;
}

static uint32_t trace_active_surface_for(const pcb_t *target)
{
    uint32_t selected = target->trace_surface;
    uint32_t mask = trace_surface_mask_for(target);

    if (selected == PPAP_TRACE_SURFACE_REAL &&
        (mask & PPAP_PTRACE_SURFACE_MASK_REAL))
        return PPAP_TRACE_SURFACE_REAL;
    if (selected == PPAP_TRACE_SURFACE_ECPU &&
        (mask & PPAP_PTRACE_SURFACE_MASK_ECPU))
        return PPAP_TRACE_SURFACE_ECPU;
    return trace_default_surface_for(target);
}

static int trace_set_surface(pcb_t *target, uint32_t surface)
{
    uint32_t mask = trace_surface_mask_for(target);

    if (target->state != PROC_TRACED_STOP)
        return -EBUSY;
    if (surface == PPAP_TRACE_SURFACE_REAL) {
        if (!(mask & PPAP_PTRACE_SURFACE_MASK_REAL))
            return -ENOSYS;
        target->trace_surface = PPAP_TRACE_SURFACE_REAL;
        target->trace_step_pending = 0;
        target->trace_swbp_skip_once = 0;
        return 0;
    }
    if (surface == PPAP_TRACE_SURFACE_ECPU) {
        if (!(mask & PPAP_PTRACE_SURFACE_MASK_ECPU))
            return -ENOSYS;
        target->trace_surface = PPAP_TRACE_SURFACE_ECPU;
        target->trace_step_pending = 0;
        target->trace_swbp_skip_once = 0;
        return 0;
    }
    return -EINVAL;
}

static uint32_t trace_regset_for(const pcb_t *target)
{
    if (trace_active_surface_for(target) == PPAP_TRACE_SURFACE_ECPU) {
        if (target->subsys == SUBSYS_CPM)
            return PPAP_TRACE_REGSET_Z80;
        if (target->subsys == SUBSYS_PPAP && target->subsys_data)
            return PPAP_TRACE_REGSET_M68K;
    }

#if defined(__m68k__)
    return PPAP_TRACE_REGSET_M68K;
#else
    return PPAP_TRACE_REGSET_ARM;
#endif
}

static int trace_store_arm_regs(pcb_t *target, const struct ppap_ptrace_regs *regs)
{
    uint32_t *sp = (uint32_t *)(uintptr_t)target->sp;
    uint32_t expected_sp;

    if (regs->regset != PPAP_TRACE_REGSET_ARM)
        return -EINVAL;
    if (regs->words < 17 || regs->words > PPAP_PTRACE_REGS_MAX)
        return -EINVAL;

    expected_sp = (uint32_t)(uintptr_t)(sp + 16);
    if (regs->regs[13] != expected_sp)
        return -ENOSYS;

    sp[8] = regs->regs[0];
    sp[9] = regs->regs[1];
    sp[10] = regs->regs[2];
    sp[11] = regs->regs[3];
    sp[0] = regs->regs[4];
    sp[1] = regs->regs[5];
    sp[2] = regs->regs[6];
    sp[3] = regs->regs[7];
    sp[4] = regs->regs[8];
    sp[5] = regs->regs[9];
    sp[6] = regs->regs[10];
    sp[7] = regs->regs[11];
    sp[12] = regs->regs[12];
    sp[13] = regs->regs[14];
    sp[14] = regs->regs[15];
    sp[15] = regs->regs[16];
    return 0;
}

#if defined(__m68k__)
static int trace_store_m68k_frame_regs(pcb_t *target,
                                       const struct ppap_ptrace_regs *regs)
{
    uint32_t *frame = (uint32_t *)(uintptr_t)target->sp;
    uint8_t *exc = (uint8_t *)(uintptr_t)target->sp + 60;
    uint32_t expected_ksp = target->sp;
    uint32_t pc;
    uint16_t sr;

    if (regs->regset != PPAP_TRACE_REGSET_M68K)
        return -EINVAL;
    if (regs->words < 20 || regs->words > PPAP_PTRACE_REGS_MAX)
        return -EINVAL;
    if (regs->regs[19] != expected_ksp)
        return -ENOSYS;
    if (regs->regs[15] != regs->regs[18])
        return -EINVAL;

    for (uint32_t i = 0; i < 15; i++)
        frame[i] = regs->regs[i];

    target->usp = regs->regs[18];
    pc = regs->regs[16];
    sr = (uint16_t)(regs->regs[17] & 0xFFFFu);
    *(uint16_t *)(void *)exc = sr;
    *(uint16_t *)(void *)(exc + 2) = (uint16_t)(pc >> 16);
    *(uint16_t *)(void *)(exc + 4) = (uint16_t)(pc & 0xFFFFu);
    return 0;
}
#endif

static int trace_store_m68k_emu_regs(pcb_t *target,
                                     const struct ppap_ptrace_regs *regs)
{
    ppap_m68k_exec_state_t *state =
        (ppap_m68k_exec_state_t *)target->subsys_data;
    m68k_state_t *cpu;

    if (!state)
        return -EINVAL;
    if (regs->regset != PPAP_TRACE_REGSET_M68K)
        return -EINVAL;
    if (regs->words < 20 || regs->words > PPAP_PTRACE_REGS_MAX)
        return -EINVAL;

    cpu = &state->m68k;
    for (uint32_t i = 0; i < 8; i++) {
        cpu->d[i] = regs->regs[i];
        cpu->a[i] = regs->regs[8 + i];
    }
    cpu->pc = regs->regs[16];
    cpu->sr = (uint16_t)(regs->regs[17] & 0xFFFFu);
    cpu->usp = regs->regs[18];
    cpu->ssp = regs->regs[19];
    return 0;
}

static int trace_store_z80_regs(pcb_t *target, const struct ppap_ptrace_regs *regs)
{
    z80_state_t *cpu = (z80_state_t *)target->subsys_data;

    if (!cpu)
        return -EINVAL;
    if (regs->regset != PPAP_TRACE_REGSET_Z80)
        return -EINVAL;
    if (regs->words < 18 || regs->words > PPAP_PTRACE_REGS_MAX)
        return -EINVAL;

    z80_set_af(cpu, (uint16_t)(regs->regs[0] & 0xFFFFu));
    z80_set_bc(cpu, (uint16_t)(regs->regs[1] & 0xFFFFu));
    z80_set_de(cpu, (uint16_t)(regs->regs[2] & 0xFFFFu));
    z80_set_hl(cpu, (uint16_t)(regs->regs[3] & 0xFFFFu));
    cpu->ix = (uint16_t)(regs->regs[4] & 0xFFFFu);
    cpu->iy = (uint16_t)(regs->regs[5] & 0xFFFFu);
    cpu->sp = (uint16_t)(regs->regs[6] & 0xFFFFu);
    cpu->pc = (uint16_t)(regs->regs[7] & 0xFFFFu);
    cpu->a2 = (uint8_t)(regs->regs[8] >> 8);
    cpu->f2 = (uint8_t)(regs->regs[8] & 0xFFu);
    cpu->b2 = (uint8_t)(regs->regs[9] >> 8);
    cpu->c2 = (uint8_t)(regs->regs[9] & 0xFFu);
    cpu->d2 = (uint8_t)(regs->regs[10] >> 8);
    cpu->e2 = (uint8_t)(regs->regs[10] & 0xFFu);
    cpu->h2 = (uint8_t)(regs->regs[11] >> 8);
    cpu->l2 = (uint8_t)(regs->regs[11] & 0xFFu);
    cpu->iff1 = (uint8_t)(regs->regs[12] & 0x1u);
    cpu->iff2 = (uint8_t)(regs->regs[13] & 0x1u);
    cpu->im = (uint8_t)(regs->regs[14] & 0x3u);
    cpu->wz = (uint16_t)(regs->regs[15] & 0xFFFFu);
    cpu->i = (uint8_t)(regs->regs[16] & 0xFFu);
    cpu->r = (uint8_t)(regs->regs[17] & 0xFFu);
    return 0;
}

static int trace_native_contains(const pcb_t *target, uint32_t addr)
{
#if defined(__m68k__)
    if (target->user_stack_page) {
        uint32_t base = (uint32_t)(uintptr_t)target->user_stack_page;
        if (addr >= base && addr < base + PAGE_SIZE)
            return 1;
    }
#else
    if (target->stack_page) {
        uint32_t base = (uint32_t)(uintptr_t)target->stack_page;
        if (addr >= base && addr < base + PAGE_SIZE)
            return 1;
    }
#endif

    for (uint32_t i = 0; i < USER_PAGES_MAX; i++) {
        if (!target->user_pages[i])
            continue;
        uint32_t base = (uint32_t)(uintptr_t)target->user_pages[i];
        if (addr >= base && addr < base + PAGE_SIZE)
            return 1;
    }

    for (uint32_t i = 0; i < MMAP_REGIONS_MAX; i++) {
        if (!target->mmap_regions[i].addr || !target->mmap_regions[i].pages)
            continue;
        uint32_t base = (uint32_t)(uintptr_t)target->mmap_regions[i].addr;
        uint32_t end = base + target->mmap_regions[i].pages * PAGE_SIZE;
        if (addr >= base && addr < end)
            return 1;
    }

    return 0;
}

static int trace_native_read32(const pcb_t *target, uint32_t addr, uint32_t *word)
{
    const volatile uint8_t *p0;
    const volatile uint8_t *p1;
    const volatile uint8_t *p2;
    const volatile uint8_t *p3;

    if (!word)
        return -EINVAL;
    if (addr > UINT32_MAX - 3u)
        return -EFAULT;
    if (!trace_native_contains(target, addr) ||
        !trace_native_contains(target, addr + 1) ||
        !trace_native_contains(target, addr + 2) ||
        !trace_native_contains(target, addr + 3))
        return -EFAULT;

    p0 = (const volatile uint8_t *)(uintptr_t)addr;
    p1 = (const volatile uint8_t *)(uintptr_t)(addr + 1);
    p2 = (const volatile uint8_t *)(uintptr_t)(addr + 2);
    p3 = (const volatile uint8_t *)(uintptr_t)(addr + 3);

#if defined(__m68k__)
    *word = ((uint32_t)*p0 << 24) | ((uint32_t)*p1 << 16)
          | ((uint32_t)*p2 << 8) | (uint32_t)*p3;
#else
    *word = (uint32_t)*p0 | ((uint32_t)*p1 << 8)
          | ((uint32_t)*p2 << 16) | ((uint32_t)*p3 << 24);
#endif
    return 0;
}

static int trace_native_write32(const pcb_t *target, uint32_t addr, uint32_t word)
{
    volatile uint8_t *p0;
    volatile uint8_t *p1;
    volatile uint8_t *p2;
    volatile uint8_t *p3;

    if (addr > UINT32_MAX - 3u)
        return -EFAULT;
    if (!trace_native_contains(target, addr) ||
        !trace_native_contains(target, addr + 1) ||
        !trace_native_contains(target, addr + 2) ||
        !trace_native_contains(target, addr + 3))
        return -EFAULT;

    p0 = (volatile uint8_t *)(uintptr_t)addr;
    p1 = (volatile uint8_t *)(uintptr_t)(addr + 1);
    p2 = (volatile uint8_t *)(uintptr_t)(addr + 2);
    p3 = (volatile uint8_t *)(uintptr_t)(addr + 3);

#if defined(__m68k__)
    *p0 = (uint8_t)(word >> 24);
    *p1 = (uint8_t)(word >> 16);
    *p2 = (uint8_t)(word >> 8);
    *p3 = (uint8_t)word;
#else
    *p0 = (uint8_t)word;
    *p1 = (uint8_t)(word >> 8);
    *p2 = (uint8_t)(word >> 16);
    *p3 = (uint8_t)(word >> 24);
#endif
    return 0;
}

static int trace_m68k_emu_read32(const pcb_t *target, uint32_t addr, uint32_t *word)
{
    const ppap_m68k_exec_state_t *state =
        (const ppap_m68k_exec_state_t *)target->subsys_data;

    if (!state || !word)
        return -EINVAL;
    if (addr > state->m68k.mem_size || state->m68k.mem_size - addr < 4)
        return -EFAULT;

    *word = m68k_read32((m68k_state_t *)&state->m68k, addr);
    return 0;
}

static int trace_m68k_emu_write32(const pcb_t *target, uint32_t addr, uint32_t word)
{
    const ppap_m68k_exec_state_t *state =
        (const ppap_m68k_exec_state_t *)target->subsys_data;

    if (!state)
        return -EINVAL;
    if (addr > state->m68k.mem_size || state->m68k.mem_size - addr < 4)
        return -EFAULT;

    m68k_write32((m68k_state_t *)&state->m68k, addr, word);
    return 0;
}

static int trace_z80_read32(const pcb_t *target, uint32_t addr, uint32_t *word)
{
    const z80_state_t *cpu = (const z80_state_t *)target->subsys_data;

    if (!cpu || !word)
        return -EINVAL;
    if (addr > cpu->mem_size || cpu->mem_size - addr < 4)
        return -EFAULT;

    *word = (uint32_t)cpu->memory[addr]
          | ((uint32_t)cpu->memory[addr + 1] << 8)
          | ((uint32_t)cpu->memory[addr + 2] << 16)
          | ((uint32_t)cpu->memory[addr + 3] << 24);
    return 0;
}

static int trace_z80_write32(const pcb_t *target, uint32_t addr, uint32_t word)
{
    z80_state_t *cpu = (z80_state_t *)target->subsys_data;

    if (!cpu)
        return -EINVAL;
    if (addr > cpu->mem_size || cpu->mem_size - addr < 4)
        return -EFAULT;

    cpu->memory[addr] = (uint8_t)word;
    cpu->memory[addr + 1] = (uint8_t)(word >> 8);
    cpu->memory[addr + 2] = (uint8_t)(word >> 16);
    cpu->memory[addr + 3] = (uint8_t)(word >> 24);
    return 0;
}

static int trace_read32(const pcb_t *target, uint32_t addr, uint32_t *word)
{
    if (target->state != PROC_TRACED_STOP)
        return -EBUSY;

    if (trace_active_surface_for(target) == PPAP_TRACE_SURFACE_ECPU) {
        if (target->subsys == SUBSYS_CPM)
            return trace_z80_read32(target, addr, word);
        if (target->subsys == SUBSYS_PPAP && target->subsys_data)
            return trace_m68k_emu_read32(target, addr, word);
        return -ENOSYS;
    }

    return trace_native_read32(target, addr, word);
}

static int trace_write32(const pcb_t *target, uint32_t addr, uint32_t word)
{
    if (target->state != PROC_TRACED_STOP)
        return -EBUSY;

    if (trace_active_surface_for(target) == PPAP_TRACE_SURFACE_ECPU) {
        if (target->subsys == SUBSYS_CPM)
            return trace_z80_write32(target, addr, word);
        if (target->subsys == SUBSYS_PPAP && target->subsys_data)
            return trace_m68k_emu_write32(target, addr, word);
        return -ENOSYS;
    }

    return trace_native_write32(target, addr, word);
}

static int trace_fill_regs(const pcb_t *target, struct ppap_ptrace_regs *regs)
{
    if (target->state != PROC_TRACED_STOP)
        return -EBUSY;

    if (trace_active_surface_for(target) == PPAP_TRACE_SURFACE_ECPU) {
        if (target->subsys == SUBSYS_CPM)
            return trace_fill_z80_regs(target, regs);
        if (target->subsys == SUBSYS_PPAP && target->subsys_data)
            return trace_fill_m68k_emu_regs(target, regs);
        return -ENOSYS;
    }

#if defined(__m68k__)
    return trace_fill_m68k_frame_regs(target, regs);
#else
    return trace_fill_arm_regs(target, regs);
#endif
}

static int trace_store_regs(pcb_t *target, const struct ppap_ptrace_regs *regs)
{
    if (!regs)
        return -EINVAL;
    if (target->state != PROC_TRACED_STOP)
        return -EBUSY;

    if (trace_active_surface_for(target) == PPAP_TRACE_SURFACE_ECPU) {
        if (target->subsys == SUBSYS_CPM)
            return trace_store_z80_regs(target, regs);
        if (target->subsys == SUBSYS_PPAP && target->subsys_data)
            return trace_store_m68k_emu_regs(target, regs);
        return -ENOSYS;
    }

#if defined(__m68k__)
    return trace_store_m68k_frame_regs(target, regs);
#else
    return trace_store_arm_regs(target, regs);
#endif
}

static int trace_fill_caps(const pcb_t *target, struct ppap_ptrace_caps *caps)
{
    uint32_t c = PPAP_PTRACE_CAP_GETREGS
               | PPAP_PTRACE_CAP_SETREGS
               | PPAP_PTRACE_CAP_PEEKPOKE;
    uint32_t surface;
    uint32_t surfaces;

    if (!caps)
        return -EINVAL;
    if (target->state != PROC_TRACED_STOP)
        return -EBUSY;

    surface = trace_active_surface_for(target);
    surfaces = trace_surface_mask_for(target);
    if (surface == PPAP_TRACE_SURFACE_ECPU) {
        if (target->subsys == SUBSYS_CPM)
            c |= PPAP_PTRACE_CAP_SINGLESTEP | PPAP_PTRACE_CAP_SW_BP;
        if (target->subsys == SUBSYS_PPAP && target->subsys_data)
            c |= PPAP_PTRACE_CAP_SINGLESTEP | PPAP_PTRACE_CAP_SW_BP;
    }

    caps->regset = trace_regset_for(target);
    caps->abi = target->trace_event.abi;
    caps->surface = surface;
    caps->surfaces = surfaces;
    caps->caps = c;
    caps->max_bps = (c & PPAP_PTRACE_CAP_SW_BP) ? TRACE_SW_BP_MAX : 0;
    return 0;
}

static int trace_supports_single_step(const pcb_t *target)
{
    if (trace_active_surface_for(target) == PPAP_TRACE_SURFACE_ECPU) {
        if (target->subsys == SUBSYS_CPM)
            return target->subsys_data != 0;
        if (target->subsys == SUBSYS_PPAP && target->subsys_data)
            return 1;
    }

    return 0;
}

static int trace_request_single_step(pcb_t *target)
{
    if (target->state != PROC_TRACED_STOP)
        return -EBUSY;
    if (!trace_supports_single_step(target))
        return -ENOSYS;

    target->trace_step_pending = 1;
    trace_resume_target(target);
    return 0;
}

static void trace_clear_swbp(pcb_t *target)
{
    target->trace_swbp_skip_once = 0;
    target->trace_swbp_skip_pc = 0;
    for (uint32_t i = 0; i < TRACE_SW_BP_MAX; i++) {
        target->trace_swbp[i].addr = 0;
        target->trace_swbp[i].used = 0;
        target->trace_swbp[i].enabled = 0;
    }
}

static int trace_supports_swbp(const pcb_t *target)
{
    if (trace_active_surface_for(target) != PPAP_TRACE_SURFACE_ECPU)
        return 0;
    if (target->subsys == SUBSYS_CPM)
        return target->subsys_data != 0;
    if (target->subsys == SUBSYS_PPAP && target->subsys_data)
        return 1;
    return 0;
}

int trace_has_swbp(void)
{
    if (trace_active_surface_for(current) != PPAP_TRACE_SURFACE_ECPU)
        return 0;
    for (uint32_t i = 0; i < TRACE_SW_BP_MAX; i++) {
        if (current->trace_swbp[i].used && current->trace_swbp[i].enabled)
            return 1;
    }
    return 0;
}

static int trace_set_swbp(pcb_t *target, struct ppap_ptrace_bp *bp)
{
    if (!bp)
        return -EINVAL;
    if (target->state != PROC_TRACED_STOP)
        return -EBUSY;
    if (!trace_supports_swbp(target))
        return -ENOSYS;
    if (bp->flags != 0 && bp->flags != PPAP_PTRACE_BP_SW)
        return -EINVAL;

    for (int32_t i = 0; i < TRACE_SW_BP_MAX; i++) {
        if (target->trace_swbp[i].used &&
            target->trace_swbp[i].enabled &&
            target->trace_swbp[i].addr == bp->addr) {
            bp->id = i;
            bp->flags = PPAP_PTRACE_BP_SW;
            return 0;
        }
    }

    for (int32_t i = 0; i < TRACE_SW_BP_MAX; i++) {
        if (target->trace_swbp[i].used)
            continue;
        target->trace_swbp[i].addr = bp->addr;
        target->trace_swbp[i].used = 1;
        target->trace_swbp[i].enabled = 1;
        bp->id = i;
        bp->flags = PPAP_PTRACE_BP_SW;
        return 0;
    }

    return -ENOSPC;
}

static int trace_clr_swbp(pcb_t *target, struct ppap_ptrace_bp *bp)
{
    int32_t id;

    if (!bp)
        return -EINVAL;
    if (target->state != PROC_TRACED_STOP)
        return -EBUSY;
    if (!trace_supports_swbp(target))
        return -ENOSYS;

    id = bp->id;
    if (id < 0 || id >= TRACE_SW_BP_MAX)
        return -EINVAL;
    if (!target->trace_swbp[id].used)
        return -EINVAL;

    if (target->trace_swbp_skip_once &&
        target->trace_swbp_skip_pc == target->trace_swbp[id].addr) {
        target->trace_swbp_skip_once = 0;
        target->trace_swbp_skip_pc = 0;
    }

    target->trace_swbp[id].addr = 0;
    target->trace_swbp[id].used = 0;
    target->trace_swbp[id].enabled = 0;
    return 0;
}

int trace_before_syscall(uint32_t *frame, uint32_t nr, uint32_t a4, uint32_t a5)
{
    if (!(current->trace_mode & PPAP_TRACE_MODE_PPAP_SYSCALL))
        return 0;
    if (current->trace_syscall_phase != TRACE_PHASE_ENTER)
        return 0;
    if (nr == SYS_PTRACE)
        return 0;

    current->trace_syscall_phase = TRACE_PHASE_EXIT;
    trace_fill_event(PPAP_TRACE_EVENT_SYSCALL_ENTER, PPAP_TRACE_ABI_PPAP, nr,
                     frame[0], frame[1], frame[2], frame[3], a4, a5, 0, 0);
    trace_stop_current(1);
    return 1;
}

void trace_after_syscall(uint32_t *frame, uint32_t nr,
                         uint32_t a4, uint32_t a5, long ret)
{
    if (!(current->trace_mode & PPAP_TRACE_MODE_PPAP_SYSCALL))
        return;
    if (current->trace_syscall_phase != TRACE_PHASE_EXIT)
        return;
    if (current->state != PROC_RUNNABLE)
        return;
    if (nr == SYS_PTRACE)
        return;

    current->trace_syscall_phase = TRACE_PHASE_ENTER;
    trace_fill_event(PPAP_TRACE_EVENT_SYSCALL_EXIT, PPAP_TRACE_ABI_PPAP, nr,
                     frame[0], frame[1], frame[2], frame[3], a4, a5,
                     (int32_t)ret, 0);
    trace_stop_current(0);
}

int trace_before_subsys(uint32_t abi, uint32_t nr,
                        uint32_t a0, uint32_t a1, uint32_t a2,
                        uint32_t a3, uint32_t a4, uint32_t a5)
{
    if (!(current->trace_mode & PPAP_TRACE_MODE_SUBSYS_CALL))
        return 0;
    if (current->trace_subsys_phase != TRACE_PHASE_ENTER)
        return 0;

    current->trace_subsys_phase = TRACE_PHASE_EXIT;
    trace_fill_event(PPAP_TRACE_EVENT_SUBSYS_ENTER, abi, nr,
                     a0, a1, a2, a3, a4, a5, 0, 0);
    trace_stop_current(0);
    return 1;
}

void trace_after_subsys(uint32_t abi, uint32_t nr,
                        uint32_t a0, uint32_t a1, uint32_t a2,
                        uint32_t a3, uint32_t a4, uint32_t a5,
                        int32_t ret)
{
    if (!(current->trace_mode & PPAP_TRACE_MODE_SUBSYS_CALL))
        return;
    if (current->trace_subsys_phase != TRACE_PHASE_EXIT)
        return;
    if (current->state != PROC_RUNNABLE)
        return;

    current->trace_subsys_phase = TRACE_PHASE_ENTER;
    trace_fill_event(PPAP_TRACE_EVENT_SUBSYS_EXIT, abi, nr,
                     a0, a1, a2, a3, a4, a5, ret, 0);
    trace_stop_current(0);
}

void trace_debug_stop(uint32_t abi, uint32_t pc, uint32_t flags)
{
    trace_fill_event(PPAP_TRACE_EVENT_DEBUG_STOP, abi, 0,
                     pc, 0, 0, 0, 0, 0, 0, flags);
    trace_stop_current(0);
}

int trace_maybe_stop_at_swbp(uint32_t abi, uint32_t pc)
{
    if (!current->tracer_pid)
        return 0;
    if (current->state != PROC_RUNNABLE)
        return 0;
    if (trace_active_surface_for(current) != PPAP_TRACE_SURFACE_ECPU)
        return 0;

    if (current->trace_swbp_skip_once) {
        if (current->trace_swbp_skip_pc == pc) {
            current->trace_swbp_skip_once = 0;
            return 0;
        }
        current->trace_swbp_skip_once = 0;
    }

    for (uint32_t i = 0; i < TRACE_SW_BP_MAX; i++) {
        if (!current->trace_swbp[i].used || !current->trace_swbp[i].enabled)
            continue;
        if (current->trace_swbp[i].addr != pc)
            continue;
        current->trace_swbp_skip_once = 1;
        current->trace_swbp_skip_pc = pc;
        trace_debug_stop(abi, pc, PPAP_DEBUG_STOP_SW_BP);
        return 1;
    }
    return 0;
}

void trace_exec_stop(void)
{
    if (!current->trace_requested)
        return;

    current->trace_requested = 0;
    current->trace_syscall_phase = TRACE_PHASE_ENTER;
    current->trace_subsys_phase = TRACE_PHASE_ENTER;
    current->trace_step_pending = 0;
    current->trace_surface = (uint8_t)trace_default_surface_for(current);
    trace_clear_swbp(current);
    trace_fill_event(PPAP_TRACE_EVENT_EXEC, PPAP_TRACE_ABI_PPAP, SYS_EXECVE,
                     0, 0, 0, 0, 0, 0, 0, 0);
    trace_stop_current(0);
}

long sys_ptrace(long req, long pid, void *addr, void *data)
{
    if (req == PTRACE_TRACEME) {
        if (current->tracer_pid != 0 || current->trace_requested)
            return -(long)EPERM;
        current->tracer_pid = current->ppid;
        current->trace_requested = 1;
        current->trace_mode = 0;
        current->trace_surface = PPAP_TRACE_SURFACE_REAL;
        current->trace_wait_pending = 0;
        current->trace_syscall_phase = TRACE_PHASE_ENTER;
        current->trace_subsys_phase = TRACE_PHASE_ENTER;
        current->trace_step_pending = 0;
        trace_clear_swbp(current);
        __builtin_memset(&current->trace_event, 0, sizeof(current->trace_event));
        return 0;
    }

    if (req == PTRACE_ATTACH) {
        pcb_t *target = trace_find_process(pid);
        if (!target || target->state == PROC_ZOMBIE)
            return -(long)ESRCH;
        if (target == current)
            return -(long)EPERM;
        if (target->tracer_pid != 0 || target->trace_requested)
            return -(long)EPERM;

        target->tracer_pid = current->pid;
        target->trace_requested = 0;
        trace_reset_mode_state(target, 0);
        target->trace_surface = (uint8_t)trace_default_surface_for(target);
        target->trace_wait_pending = 1;
        target->trace_step_pending = 0;
        trace_clear_swbp(target);
        __builtin_memset(&target->trace_event, 0, sizeof(target->trace_event));
        target->trace_event.event = PPAP_TRACE_EVENT_DEBUG_STOP;
        target->trace_event.abi = PPAP_TRACE_ABI_PPAP;
        target->state = PROC_TRACED_STOP;
        trace_wake_tracer(target);
        return 0;
    }

    pcb_t *target = trace_find_tracee(current->pid, pid);
    if (!target)
        return -(long)ESRCH;

    switch (req) {
    case PTRACE_GETEVENT:
        if (!data)
            return -(long)EINVAL;
        *(struct ppap_ptrace_event *)data = target->trace_event;
        return 0;
    case PTRACE_PEEKDATA:
        if (!data)
            return -(long)EINVAL;
        return trace_read32(target, (uint32_t)(uintptr_t)addr,
                            (uint32_t *)data);
    case PTRACE_POKEDATA:
        if (!data)
            return -(long)EINVAL;
        return trace_write32(target, (uint32_t)(uintptr_t)addr,
                             *(const uint32_t *)data);
    case PTRACE_GETREGS:
        if (!data)
            return -(long)EINVAL;
        return trace_fill_regs(target, (struct ppap_ptrace_regs *)data);
    case PTRACE_SETREGS:
        if (!data)
            return -(long)EINVAL;
        {
            int rc = trace_store_regs(target,
                                      (const struct ppap_ptrace_regs *)data);
            if (rc == 0)
                target->trace_swbp_skip_once = 0;
            return rc;
        }
    case PTRACE_GETCAPS:
        if (!data)
            return -(long)EINVAL;
        return trace_fill_caps(target, (struct ppap_ptrace_caps *)data);
    case PTRACE_GETSURFACE:
        if (!data)
            return -(long)EINVAL;
        *(uint32_t *)data = trace_active_surface_for(target);
        return 0;
    case PTRACE_SETSURFACE:
        return trace_set_surface(target, (uint32_t)(uintptr_t)addr);
    case PTRACE_SETMODE: {
        uint8_t mode = (uint8_t)(uintptr_t)addr;
        if (mode & (uint8_t)~TRACE_MODE_MASK)
            return -(long)EINVAL;
        trace_reset_mode_state(target, mode);
        return 0;
    }
    case PTRACE_SETBP:
        if (!data)
            return -(long)EINVAL;
        return trace_set_swbp(target, (struct ppap_ptrace_bp *)data);
    case PTRACE_CLRBP:
        if (!data)
            return -(long)EINVAL;
        return trace_clr_swbp(target, (struct ppap_ptrace_bp *)data);
    case PTRACE_SINGLESTEP:
        return trace_request_single_step(target);
    case PTRACE_CONT:
        target->trace_step_pending = 0;
        trace_resume_target(target);
        return 0;
    case PTRACE_SYSCALL:
        target->trace_step_pending = 0;
        if (!(target->trace_mode & PPAP_TRACE_MODE_PPAP_SYSCALL))
            target->trace_syscall_phase = TRACE_PHASE_ENTER;
        target->trace_mode |= PPAP_TRACE_MODE_PPAP_SYSCALL;
        trace_resume_target(target);
        return 0;
    case PTRACE_DETACH:
        target->trace_requested = 0;
        target->tracer_pid = 0;
        trace_reset_mode_state(target, 0);
        target->trace_surface = (uint8_t)trace_default_surface_for(target);
        target->trace_wait_pending = 0;
        target->trace_step_pending = 0;
        trace_clear_swbp(target);
        __builtin_memset(&target->trace_event, 0, sizeof(target->trace_event));
        trace_resume_target(target);
        return 0;
    default:
        return -(long)EINVAL;
    }
}

/* ── sys_exit ───────────────────────────────────────────────────────────────── */

/*
 * Terminate the calling process:
 *   1. Store exit status for waitpid()
 *   2. Close all file descriptors
 *   3. Free user pages (if owned — not shared via vfork)
 *   4. Unblock vfork parent if applicable
 *   5. Wake parent (if blocked in waitpid)
 *   6. Reparent children to init (PID 1)
 *   7. Mark ZOMBIE and yield
 *
 * Page lifecycle:
 *   - user_pages[] are freed here in sys_exit() (step 3).
 *   - stack_page is freed later in sys_waitpid() when the parent reaps
 *     the zombie.  Orphans are reparented to init (step 6), ensuring
 *     init can always reap them.
 *
 * Note: sys_exit runs inside SVC_Handler (Handler mode).  sched_yield()
 * only pends PendSV, which tail-chains after SVC returns.  There is no
 * need for an infinite loop — the ZOMBIE process will never be scheduled
 * again because sched_next() only picks PROC_RUNNABLE processes.
 */
long sys_exit(long status)
{
    current->exit_status = (int)status;

    /* Close all open fds */
    fd_close_all(current);

    /* Free user pages only if we own them (vfork_parent == NULL means
     * either this isn't a vfork child, or execve already replaced them) */
    if (!current->vfork_parent) {
        for (int i = 0; i < USER_PAGES_MAX; i++) {
            if (current->user_pages[i]) {
                page_free(current->user_pages[i]);
                current->user_pages[i] = NULL;
            }
        }
#if defined(__m68k__)
        if (current->user_stack_page) {
            page_free(current->user_stack_page);
            current->user_stack_page = NULL;
        }
#endif
        /* Free mmap regions */
        for (int i = 0; i < MMAP_REGIONS_MAX; i++) {
            if (current->mmap_regions[i].addr) {
                uint32_t base = (uint32_t)(uintptr_t)current->mmap_regions[i].addr;
                for (uint32_t j = 0; j < current->mmap_regions[i].pages; j++)
                    page_free((void *)(uintptr_t)(base + j * PAGE_SIZE));
                current->mmap_regions[i].addr  = NULL;
                current->mmap_regions[i].pages = 0;
            }
        }
    } else {
        /* vfork child exiting without exec: free child-owned pages only
         * (e.g. the user stack copy allocated by sys_vfork). */
        for (int i = 0; i < USER_PAGES_MAX; i++) {
            if (current->user_pages[i] &&
                current->user_pages[i] !=
                    current->vfork_parent->user_pages[i]) {
                page_free(current->user_pages[i]);
                current->user_pages[i] = NULL;
            }
        }
#if defined(__m68k__)
        /* Free child's user stack copy if different from parent's */
        if (current->user_stack_page &&
            current->user_stack_page != current->vfork_parent->user_stack_page) {
            page_free(current->user_stack_page);
            current->user_stack_page = NULL;
        }
#endif
    }

    /* Unblock vfork parent if we are a vfork child */
    if (current->vfork_parent) {
        current->vfork_parent->state = PROC_RUNNABLE;
        current->vfork_parent = NULL;
    }

    /* Wake parent if it is blocked (e.g. in waitpid).
     * After execve, vfork_parent is NULL so the vfork unblock above
     * won't fire — we need this separate wake-up for waitpid. */
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].pid == current->ppid &&
            proc_table[i].state == PROC_BLOCKED) {
            proc_table[i].state = PROC_RUNNABLE;
            break;
        }
    }
    if (current->tracer_pid != 0 && current->tracer_pid != current->ppid) {
        for (uint32_t i = 0; i < PROC_MAX; i++) {
            if (proc_table[i].pid == current->tracer_pid &&
                proc_table[i].state == PROC_BLOCKED) {
                proc_table[i].state = PROC_RUNNABLE;
                break;
            }
        }
    }

    /* Reparent children to init (PID 1) so they can be reaped.
     * If a reparented child is already zombie, wake init. */
    for (uint32_t i = 1; i < PROC_MAX; i++) {
        pcb_t *child = &proc_table[i];
        if (child->state == PROC_FREE || child->ppid != current->pid)
            continue;
        child->ppid = 1;
        if (child->state == PROC_ZOMBIE) {
            for (uint32_t j = 0; j < PROC_MAX; j++) {
                if (proc_table[j].pid == 1 &&
                    proc_table[j].state == PROC_BLOCKED) {
                    proc_table[j].state = PROC_RUNNABLE;
                    break;
                }
            }
        }
    }

    current->state = PROC_ZOMBIE;
    sched_yield();
    return 0;  /* never reached — PendSV switches away after SVC returns */
}

/* ── sys_getpid ─────────────────────────────────────────────────────────────── */

long sys_getpid(void)
{
    return (long)current->pid;
}

/* ── sys_vfork ──────────────────────────────────────────────────────────────── */

/*
 * Create a child process.  The parent is blocked until the child calls
 * execve() or _exit().
 *
 * The child gets its own stack page with a copy of the parent's exception
 * frame (return register = 0 for child).  The child shares the parent's
 * user_pages (GOT/data).
 *
 * frame: pointer to the parent's stacked exception frame
 *   ARM:  [r0, r1, r2, r3, r12, lr, pc, xpsr] on PSP
 *   m68k: &regs[1] (d1 slot) in the TRAP #0 saved register frame
 */
long sys_vfork(uint32_t *frame)
{
    /* 1. Allocate child PCB */
    pcb_t *child = proc_alloc();
    if (!child)
        return -(long)ENOMEM;

    /* 2. Allocate stack page for child */
    void *stack = page_alloc();
    if (!stack) {
        proc_free(child);
        return -(long)ENOMEM;
    }
    child->stack_page = stack;

    /* 3. Share parent's user_pages with child */
    for (int i = 0; i < USER_PAGES_MAX; i++)
        child->user_pages[i] = current->user_pages[i];

    /* 4. Build child's stack: copy the parent's entire stack page.
     *
     *    The compiler saves local variables (including r9/GOT base) to
     *    the stack.  After vfork returns, the child reads these stack-saved
     *    values.  If the child has a fresh stack, those reads return garbage.
     *    Copying the parent's stack gives the child a valid snapshot.
     *
     *    Then build the PendSV context (SW + HW frames) at the same offset
     *    on the child's page as the parent's PSP frame.
     */
    memcpy(stack, current->stack_page, PAGE_SIZE);

    /* Calculate child's frame position at the same offset as parent's */
    uintptr_t frame_off = (uintptr_t)frame - (uintptr_t)current->stack_page;
    uint32_t *child_frame = (uint32_t *)((uint8_t *)stack + frame_off);

#if defined(__m68k__)
    /* m68k: The TRAP #0 frame (15 regs + SR + PC) has the same layout as
     * the switch.S context frame.  The child returns directly to user code
     * via switch.S restore (movem.l + rte), bypassing TRAP #0 cleanup.
     *
     * frame = &regs[1] (d1 slot).  child_frame - 1 = d0 slot. */
    uint32_t *child_regs = child_frame - 1;   /* d0 slot */
    child_regs[0] = 0;                        /* d0 = 0 (child return) */
    child_regs[13] = current->got_base;       /* a5 = GOT base for PIC */

    child->sp = (uint32_t)(uintptr_t)child_regs;

    /* m68k user mode: USP points to a user_stack_page (separate from
     * stack_page / SSP).  The child must have its own user stack copy;
     * otherwise the child's post-vfork code (pushing execve arguments)
     * overwrites the parent's return addresses on the shared stack. */
    {
        void *parent_ustack = current->user_stack_page;

        if (parent_ustack) {
            void *child_ustack = page_alloc();
            if (!child_ustack) {
                page_free(stack);
                proc_free(child);
                return -(long)ENOMEM;
            }
            memcpy(child_ustack, parent_ustack, PAGE_SIZE);
            child->user_stack_page = child_ustack;

            /* Adjust child USP to same offset within the new page */
            uint32_t parent_usp = current->usp;
            uint32_t usp_off = parent_usp -
                               (uint32_t)(uintptr_t)parent_ustack;
            child->usp = (uint32_t)(uintptr_t)child_ustack + usp_off;

            /* Patch a6 (frame pointer) if it points into the user stack */
            uint32_t parent_base = (uint32_t)(uintptr_t)parent_ustack;
            uint32_t child_base = (uint32_t)(uintptr_t)child_ustack;
            if (child_regs[14] >= parent_base &&
                child_regs[14] < parent_base + PAGE_SIZE) {
                child_regs[14] += child_base - parent_base;
            }
        } else {
            child->usp = current->usp;
        }
    }
#else
    /* ARM: Set child's r0 = 0 (child sees vfork return 0) */
    child_frame[0] = 0;

    /* Build SW callee-saved frame below the HW frame.
     * r9 = GOT base so PIC addressing works after PendSV restore. */
    uint32_t *sw = child_frame - 8;
    memset(sw, 0, 8 * sizeof(uint32_t));
    sw[5] = current->got_base;   /* r9 = GOT SRAM address for PIC */

    child->sp = (uint32_t)(uintptr_t)sw;
#endif
    child->ticks_remaining = PROC_DEFAULT_TICKS;

    /* 5. Set up identity and relationships */
    child->ppid = current->pid;
    child->pgid = current->pgid;   /* inherit process group (like real fork) */
    child->sid  = current->sid;    /* inherit session ID */
    child->vfork_parent = current;
    child->got_base = current->got_base;
    child->tp_value = current->tp_value;
    memcpy(child->cwd, current->cwd, sizeof(child->cwd));
    memcpy(child->comm, current->comm, sizeof(child->comm));

    /* 6. Inherit file descriptors from parent */
    fd_inherit(child, current);

    /* 7. Set parent's return value (child PID) in stacked r0 */
    frame[0] = (uint32_t)child->pid;

    /* 8. Block parent, make child runnable */
    current->state = PROC_BLOCKED;
    child->state = PROC_RUNNABLE;

    /* 9. Yield — PendSV will switch to child after SVC returns.
     * Note: sched_yield() only pends PendSV (can't preempt SVC), so the
     * code below still executes.  This is harmless — the parent is BLOCKED,
     * so PendSV tail-chains and switches it out after our SVC return. */
    sched_yield();

    /* Clear stale flags left by the child's syscalls while we were blocked.
     * exec_pending and svc_restart are global (not per-process), so a child's
     * execve or blocking read can leave flags that would corrupt our return. */
    exec_pending[core_id()] = 0;
    svc_restart[core_id()]  = 0;

    return (long)child->pid;
}

/* ── sys_waitpid ────────────────────────────────────────────────────────────── */

/*
 * Wait for a child process to exit.
 *   pid > 0:   wait for specific child
 *   pid == -1: wait for any child
 *   options & WNOHANG: return 0 immediately if no child has exited
 *
 * Returns child PID on success, -ECHILD if no children, 0 if WNOHANG.
 *
 * Blocking: since this runs inside SVC_Handler (Handler mode), we cannot
 * loop and retry — sched_yield() only pends PendSV, which cannot preempt
 * SVC.  Instead, we mark PROC_BLOCKED, set svc_restart = 1, and return.
 * SVC_Handler restores frame[0] and adjusts PC-2 so the syscall re-executes
 * when the process is rescheduled.  PendSV tail-chains after SVC returns.
 */
long sys_waitpid(long pid, long status_ptr, long options)
{
    pcb_t *zombie = NULL;
    pcb_t *stopped = NULL;
    int has_match = 0;
    int zombie_is_child = 0;

    /* Scan for matching child or traced process. */
    for (uint32_t i = 1; i < PROC_MAX; i++) {
        pcb_t *p = &proc_table[i];
        int is_child;
        int is_tracee;

        if (p->state == PROC_FREE)
            continue;
        is_child = (p->ppid == current->pid);
        is_tracee = (p->tracer_pid == current->pid);
        if (!is_child && !is_tracee)
            continue;
        if (pid > 0 && p->pid != (pid_t)pid)
            continue;

        has_match = 1;

        if ((options & WSTOPPED) &&
            p->state == PROC_TRACED_STOP &&
            p->trace_wait_pending) {
            stopped = p;
            break;
        }

        if (p->state == PROC_ZOMBIE) {
            zombie = p;
            zombie_is_child = is_child;
            break;
        }
    }

    if (stopped) {
        if (status_ptr) {
            int *sp = (int *)(uintptr_t)status_ptr;
            *sp = W_STOPCODE(SIGTRAP);
        }
        stopped->trace_wait_pending = 0;
        return (long)stopped->pid;
    }

    if (zombie) {
        pid_t cpid = zombie->pid;

        if (status_ptr) {
            int *sp = (int *)(uintptr_t)status_ptr;
            *sp = W_EXITCODE(zombie->exit_status);
        }

        if (!zombie_is_child) {
            /* Non-child tracees report exit but are reaped by their parent. */
            zombie->tracer_pid = 0;
            zombie->trace_requested = 0;
            trace_reset_mode_state(zombie, 0);
            zombie->trace_surface = (uint8_t)trace_default_surface_for(zombie);
            zombie->trace_wait_pending = 0;
            zombie->trace_step_pending = 0;
            trace_clear_swbp(zombie);
            __builtin_memset(&zombie->trace_event, 0, sizeof(zombie->trace_event));
            return (long)cpid;
        }

        /* Reap the zombie child. */
        /* Free zombie's stack page */
        if (zombie->stack_page) {
            page_free(zombie->stack_page);
            zombie->stack_page = NULL;
        }

        proc_free(zombie);
        return (long)cpid;
    }

    if (!has_match)
        return -(long)ECHILD;

    if (options & WNOHANG)
        return 0;

    /* Block and arrange for syscall restart.
     * sys_exit will wake us by setting PROC_RUNNABLE.
     * SVC_Handler will restore frame[0] and PC-2 so the SVC re-executes. */
    current->state = PROC_BLOCKED;
    set_svc_restart();
    sched_yield();
    return 0;  /* value ignored — SVC_Handler restores original frame[0] */
}

/* ── sys_execve ─────────────────────────────────────────────────────────────── */

/*
 * Replace the current process image with a new ELF binary.
 * Called from user space (typically after vfork).
 *
 * On success: never returns — the new program starts executing.
 * On failure: returns negative errno.
 */
long sys_execve(const char *path, const char *const *argv)
{
    /* Save old pages to free after successful load */
    void *old_stack = current->stack_page;
    void *old_user[USER_PAGES_MAX];
    int owns_pages = (current->vfork_parent == NULL);
    for (int i = 0; i < USER_PAGES_MAX; i++)
        old_user[i] = current->user_pages[i];
#if defined(__m68k__)
    void *old_user_stack = current->user_stack_page;
#endif

    /* Clear pages so do_execve allocates fresh ones */
    current->stack_page = NULL;
    for (int i = 0; i < USER_PAGES_MAX; i++)
        current->user_pages[i] = NULL;
#if defined(__m68k__)
    current->user_stack_page = NULL;
#endif

    /* Load the new binary.  argv points into the old stack/data pages
     * which are still valid (detached from current but not yet freed). */
    int err = do_execve(current, path, argv);
    if (err < 0) {
        /* Restore old pages on failure — fds are untouched (POSIX) */
        current->stack_page = old_stack;
        for (int i = 0; i < USER_PAGES_MAX; i++)
            current->user_pages[i] = old_user[i];
#if defined(__m68k__)
        current->user_stack_page = old_user_stack;
#endif
        return (long)err;
    }

    /* POSIX: preserve open fds across execve (redirections, pipes).
     * Only install default tty stdio if fd 0/1/2 are not already open
     * (e.g. init's first exec before any shell sets up fds). */
    if (!current->fd_table[0] && !current->fd_table[1] &&
        !current->fd_table[2])
        fd_stdio_init(current);

    /* Free old stack page.
     * m68k has no MSP/PSP split — we're still executing on old_stack.
     * Defer the free until trap.S switches SP to the new stack. */
#if defined(__m68k__)
    extern volatile void *m68k_exec_old_stack;
    m68k_exec_old_stack = old_stack;
#else
    if (old_stack)
        page_free(old_stack);
#endif

    /* Free old user pages only if we owned them */
    if (owns_pages) {
        for (int i = 0; i < USER_PAGES_MAX; i++) {
            if (old_user[i])
                page_free(old_user[i]);
        }
#if defined(__m68k__)
        if (old_user_stack)
            page_free(old_user_stack);
#endif
    } else if (current->vfork_parent) {
        /* vfork child: free pages that were allocated specifically for
         * the child (e.g. user stack copy), not the shared parent pages. */
        for (int i = 0; i < USER_PAGES_MAX; i++) {
            if (old_user[i] &&
                old_user[i] != current->vfork_parent->user_pages[i])
                page_free(old_user[i]);
        }
#if defined(__m68k__)
        if (old_user_stack &&
            old_user_stack != current->vfork_parent->user_stack_page)
            page_free(old_user_stack);
#endif
    }

    /* Unblock vfork parent — we have our own pages now */
    if (current->vfork_parent) {
        current->vfork_parent->state = PROC_RUNNABLE;
        current->vfork_parent = NULL;
    }

    trace_exec_stop();

    /* Signal SVC_Handler to do a PendSV-like full context restore from
     * current->sp before exception return.  This ensures r4-r11 (including
     * r9/GOT base) are correctly loaded from the new process's SW frame.
     *
     * We cannot set r9 via inline asm here because the C compiler's
     * function epilogue (callee-saved register restores) would undo it. */
    exec_pending[core_id()] = 1;

    return 0;
}

/* ── sys_set_tid_address ───────────────────────────────────────────────────── */

long sys_set_tid_address(void *tidptr)
{
    current->clear_child_tid = (int *)tidptr;
    return (long)current->pid;
}

/* ── sys_uname ──────────────────────────────────────────────────────────────── */

/*
 * struct utsname layout (65 bytes per field × 6 fields = 390 bytes).
 * Matches Linux/musl: each field is char[65].
 */
#define UTS_LEN 65

long sys_uname(void *buf)
{
    if (!buf)
        return -(long)EINVAL;

    char *p = (char *)buf;
    __builtin_memset(p, 0, UTS_LEN * 6);

    /* sysname */
    const char *s = "PiPAPo";
    for (int i = 0; s[i] && i < UTS_LEN - 1; i++) p[i] = s[i];
    p += UTS_LEN;

    /* nodename — target-specific (e.g. "pico1", "pico1calc", "qemu_arm") */
    s = target_name();
    for (int i = 0; s[i] && i < UTS_LEN - 1; i++) p[i] = s[i];
    p += UTS_LEN;

    /* release */
    s = "0.11.0";
    for (int i = 0; s[i] && i < UTS_LEN - 1; i++) p[i] = s[i];
    p += UTS_LEN;

    /* version */
    s = "#1 PPAP";
    for (int i = 0; s[i] && i < UTS_LEN - 1; i++) p[i] = s[i];
    p += UTS_LEN;

    /* machine */
#if defined(__m68k__)
    s = "m68k";
#else
    s = "armv6m";
#endif
    for (int i = 0; s[i] && i < UTS_LEN - 1; i++) p[i] = s[i];
    /* p += UTS_LEN; — domainname follows but we leave it zeroed */

    return 0;
}

/* ── sys_setpgid ────────────────────────────────────────────────────────────── */

long sys_setpgid(long pid, long pgid)
{
    pcb_t *target;

    if (pid == 0)
        target = current;
    else {
        target = NULL;
        for (uint32_t i = 0; i < PROC_MAX; i++) {
            if (proc_table[i].state != PROC_FREE &&
                proc_table[i].pid == (pid_t)pid) {
                target = &proc_table[i];
                break;
            }
        }
        if (!target)
            return -(long)ESRCH;
    }

    target->pgid = (pgid == 0) ? target->pid : (pid_t)pgid;
    return 0;
}

/* ── sys_setsid ─────────────────────────────────────────────────────────────── */

long sys_setsid(void)
{
    current->sid  = current->pid;
    current->pgid = current->pid;
    return (long)current->pid;
}

/* ── sys_wait4 ──────────────────────────────────────────────────────────────── */

long sys_wait4(long pid, long status_ptr, long options, void *rusage)
{
    (void)rusage;   /* rusage not supported */
    return sys_waitpid(pid, status_ptr, options);
}
