/*
 * sys_proc.c — Process-related syscall implementations
 *
 *   sys_exit(status)    — terminate the calling process
 *   sys_getpid()        — return the calling process's PID
 *   sys_vfork(frame)    — create child process (parent blocked)
 *   sys_waitpid(pid,st) — wait for child to exit, reap zombie
 *   sys_execve(path,argv) — replace process image with new ELF binary
 */

#include <string.h>

#include "../../target/target.h"
#include "../cpu/ecpu_m68k.h"
#include "../cpu/ecpu_z80.h"
#include "../common/errno.h"
#include "../exec/exec.h"
#include "../common/mod/mod_vfs.h"
#include "../exec/exec.h"
#include "../common/mod/mod_vfs.h"
#include "../klog.h"
#include "../mm/mem_region.h"
#include "../mm/page.h"
#include "../proc/proc.h"
#include "../proc/sched.h"
#include "../signal/signal.h"
#include "../subsys/ppap_m68k_bridge.h"
#include "../subsys/subsys.h"
#include "arch/arch.h"
#include "common/ptrace.h"
#include "common/wait.h"
#include "kernel/cpu/cpu.h"
#include "syscall.h"

/* Wait status encoding (POSIX-compatible) */
#define W_EXITCODE(ret) (((ret)&0xff) << 8)
#define W_STOPCODE(sig) ((((sig)&0xff) << 8) | 0x7f)

#define TRACE_PHASE_ENTER 0
#define TRACE_PHASE_EXIT 1
#define TRACE_MODE_MASK \
  (PPAP_TRACE_MODE_PPAP_SYSCALL | PPAP_TRACE_MODE_SUBSYS_CALL)

/* Release an image segment if it is OWNED (independently allocated).
 * Non-OWNED segments (XIP, sub-pointers into another allocation) are
 * just cleared.  Data regions are no longer OWNED — they are freed via
 * proc_release_tracked_pages(), so no overlap check is needed. */
static void image_segment_release_owned(proc_image_segment_t *seg) {
  if (!seg || !seg->base) return;
  if (seg->flags & PROC_IMAGE_SEG_OWNED)
    mem_region_free(seg);
  *seg = (proc_image_segment_t){0};
}

static void image_release_owned_segments(proc_image_t *image) {
  if (!image) return;
  image_segment_release_owned(&image->text);
  image_segment_release_owned(&image->staged_text);
  image_segment_release_owned(&image->staged_rodata);
  image_segment_release_owned(&image->literal);
  image_segment_release_owned(&image->rodata);
  image_segment_release_owned(&image->data);
}

static void proc_release_stack_page(void **page) {
  proc_image_segment_t seg;

  if (!page || !*page) return;
  seg = proc_image_segment_make(*page, PAGE_SIZE, PPAP_MEM_RAM_STACK,
                                PROC_IMAGE_SEG_WRITABLE);
  mem_region_free(&seg);
  *page = NULL;
}

#if defined(__m68k__) || defined(__riscv) || defined(__xtensa__)
/* Allocate a user-stack copy for a vfork child.
 *
 * The child must have its own user stack page.  Without this, the
 * child's post-vfork code overwrites the parent's stack, corrupting
 * the parent's return state.
 *
 * Returns 0 on success, -ENOMEM on failure.  On success, *out_ustack
 * is the new page and *usp_inout is adjusted to the same offset
 * within the new page. */
static int vfork_copy_user_stack(void *parent_ustack, uint32_t parent_usp,
                                 void **out_ustack, uint32_t *usp_out) {
  proc_image_segment_t ustack_region = {0};
  if (mem_region_alloc(&ustack_region, PPAP_MEM_RAM_STACK, PAGE_SIZE,
                       PROC_IMAGE_SEG_WRITABLE) < 0)
    return -(int)ENOMEM;
  void *child_ustack = ustack_region.base;
  memcpy(child_ustack, parent_ustack, PAGE_SIZE);
  *out_ustack = child_ustack;
  uint32_t usp_off = parent_usp - (uint32_t)(uintptr_t)parent_ustack;
  *usp_out = (uint32_t)(uintptr_t)child_ustack + usp_off;
  return 0;
}
#endif /* __m68k__ || __riscv || __xtensa__ */

static void trace_clear_swbp(pcb_t *target);
static void trace_clear_hwbp(pcb_t *target);
static int trace_has_hwbp_for(const pcb_t *target);
static void trace_m68k_update_trace_bit(pcb_t *target);
static void trace_m68k_set_trace_bit(pcb_t *target, int enable);
#if defined(__m68k__) || defined(__ARM_ARCH) || defined(__arm__) || \
    defined(__thumb__)
static int trace_hwbp_hit(const pcb_t *target, uint32_t pc);
#endif

static pcb_t *trace_find_tracee(pid_t tracer_pid, long pid) {
  if (pid <= 0) return NULL;
  for (uint32_t i = 1; i < PROC_MAX; i++) {
    pcb_t *p = &proc_table[i];
    if (p->state == PROC_FREE || p->pid != (pid_t)pid) continue;
    if (p->tracer_pid != tracer_pid) continue;
    return p;
  }
  return NULL;
}

static pcb_t *trace_find_process(long pid) {
  if (pid <= 0) return NULL;
  for (uint32_t i = 1; i < PROC_MAX; i++) {
    pcb_t *p = &proc_table[i];
    if (p->state == PROC_FREE || p->pid != (pid_t)pid) continue;
    return p;
  }
  return NULL;
}

static void trace_wake_tracer(const pcb_t *tracee) {
  for (uint32_t i = 0; i < PROC_MAX; i++) {
    pcb_t *p = &proc_table[i];
    if (p->state == PROC_FREE || p->pid != tracee->tracer_pid) continue;
    if (p->state == PROC_BLOCKED) p->state = PROC_RUNNABLE;
    break;
  }
}

static void trace_fill_event(uint32_t event, uint32_t abi, uint32_t nr,
                             uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                             uint32_t a4, uint32_t a5, int32_t ret,
                             uint32_t flags) {
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

static void trace_stop_current(int restart) {
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
  if (restart) svc_set_restart();
  sched_yield();
}

static void trace_reset_mode_state(pcb_t *p, uint8_t mode) {
  p->trace_mode = mode;
  if (!(mode & PPAP_TRACE_MODE_PPAP_SYSCALL))
    p->trace_syscall_phase = TRACE_PHASE_ENTER;
  if (!(mode & PPAP_TRACE_MODE_SUBSYS_CALL))
    p->trace_subsys_phase = TRACE_PHASE_ENTER;
}

static void trace_resume_target(pcb_t *target) {
  target->trace_wait_pending = 0;
  if (target->state == PROC_TRACED_STOP) target->state = PROC_RUNNABLE;
  sched_yield();
}

static void trace_regs_init(struct ppap_ptrace_regs *regs, uint32_t regset,
                            uint32_t abi, uint32_t words) {
  __builtin_memset(regs, 0, sizeof(*regs));
  regs->regset = regset;
  regs->abi = abi;
  regs->words = words;
}

#if !defined(__m68k__)
static int trace_fill_arm_regs(const pcb_t *target,
                               struct ppap_ptrace_regs *regs) {
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
#endif

#if defined(__m68k__)
static int trace_fill_m68k_frame_regs(const pcb_t *target,
                                      struct ppap_ptrace_regs *regs) {
  const uint32_t *frame = (const uint32_t *)(uintptr_t)target->sp;
  const uint8_t *exc = (const uint8_t *)(uintptr_t)target->sp + 60;

  trace_regs_init(regs, PPAP_TRACE_REGSET_M68K, target->trace_event.abi, 20);
  for (uint32_t i = 0; i < 15; i++) regs->regs[i] = frame[i];
  regs->regs[15] = target->usp;
  regs->regs[16] =
      ((uint32_t) * (const uint16_t *)(const void *)(exc + 2) << 16) |
      *(const uint16_t *)(const void *)(exc + 4);
  regs->regs[17] = *(const uint16_t *)(const void *)exc;
  regs->regs[18] = target->usp;
  regs->regs[19] = target->sp;
  return 0;
}
#endif

static int trace_fill_m68k_emu_regs(const pcb_t *target,
                                    struct ppap_ptrace_regs *regs) {
  const ppap_m68k_exec_state_t *state =
      (const ppap_m68k_exec_state_t *)target->subsys_data;
  const m68k_state_t *cpu;

  if (!state) return -EINVAL;
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

static int trace_fill_z80_regs(const pcb_t *target,
                               struct ppap_ptrace_regs *regs) {
  const z80_state_t *cpu = (const z80_state_t *)target->subsys_data;

  if (!cpu) return -EINVAL;

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

static uint32_t trace_surface_mask_for(const pcb_t *target) {
  uint32_t mask = PPAP_PTRACE_SURFACE_MASK_REAL;

  if (target->subsys_data) mask |= PPAP_PTRACE_SURFACE_MASK_ECPU;
  return mask;
}

static uint32_t trace_default_surface_for(const pcb_t *target) {
  if (trace_surface_mask_for(target) & PPAP_PTRACE_SURFACE_MASK_ECPU)
    return PPAP_TRACE_SURFACE_ECPU;
  return PPAP_TRACE_SURFACE_REAL;
}

static uint32_t trace_active_surface_for(const pcb_t *target) {
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

static int trace_set_surface(pcb_t *target, uint32_t surface) {
  uint32_t mask = trace_surface_mask_for(target);

  if (target->state != PROC_TRACED_STOP) return -EBUSY;
  if (surface == PPAP_TRACE_SURFACE_REAL) {
    if (!(mask & PPAP_PTRACE_SURFACE_MASK_REAL)) return -ENOSYS;
    target->trace_surface = PPAP_TRACE_SURFACE_REAL;
    target->trace_step_pending = 0;
    trace_m68k_set_trace_bit(target, 0);
    target->trace_swbp_skip_once = 0;
    return 0;
  }
  if (surface == PPAP_TRACE_SURFACE_ECPU) {
    if (!(mask & PPAP_PTRACE_SURFACE_MASK_ECPU)) return -ENOSYS;
    target->trace_surface = PPAP_TRACE_SURFACE_ECPU;
    target->trace_step_pending = 0;
    trace_m68k_set_trace_bit(target, 0);
    target->trace_swbp_skip_once = 0;
    return 0;
  }
  return -EINVAL;
}

static uint32_t trace_regset_for(const pcb_t *target) {
  if (trace_active_surface_for(target) == PPAP_TRACE_SURFACE_ECPU) {
    if (target->subsys == SUBSYS_CPM) return PPAP_TRACE_REGSET_Z80;
    if (target->subsys == SUBSYS_PPAP && target->subsys_data)
      return PPAP_TRACE_REGSET_M68K;
  }

#if defined(__m68k__)
  return PPAP_TRACE_REGSET_M68K;
#else
  return PPAP_TRACE_REGSET_ARM;
#endif
}

#if !defined(__m68k__)
static int trace_store_arm_regs(pcb_t *target,
                                const struct ppap_ptrace_regs *regs) {
  uint32_t *sp = (uint32_t *)(uintptr_t)target->sp;
  uint32_t expected_sp;

  if (regs->regset != PPAP_TRACE_REGSET_ARM) return -EINVAL;
  if (regs->words < 17 || regs->words > PPAP_PTRACE_REGS_MAX) return -EINVAL;

  expected_sp = (uint32_t)(uintptr_t)(sp + 16);
  if (regs->regs[13] != expected_sp) return -ENOSYS;

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
#endif

#if defined(__m68k__)
static int trace_store_m68k_frame_regs(pcb_t *target,
                                       const struct ppap_ptrace_regs *regs) {
  uint32_t *frame = (uint32_t *)(uintptr_t)target->sp;
  uint8_t *exc = (uint8_t *)(uintptr_t)target->sp + 60;
  uint32_t expected_ksp = target->sp;
  uint32_t pc;
  uint16_t sr;

  if (regs->regset != PPAP_TRACE_REGSET_M68K) return -EINVAL;
  if (regs->words < 20 || regs->words > PPAP_PTRACE_REGS_MAX) return -EINVAL;
  if (regs->regs[19] != expected_ksp) return -ENOSYS;
  if (regs->regs[15] != regs->regs[18]) return -EINVAL;

  for (uint32_t i = 0; i < 15; i++) frame[i] = regs->regs[i];

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
                                     const struct ppap_ptrace_regs *regs) {
  ppap_m68k_exec_state_t *state = (ppap_m68k_exec_state_t *)target->subsys_data;
  m68k_state_t *cpu;

  if (!state) return -EINVAL;
  if (regs->regset != PPAP_TRACE_REGSET_M68K) return -EINVAL;
  if (regs->words < 20 || regs->words > PPAP_PTRACE_REGS_MAX) return -EINVAL;

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

static int trace_store_z80_regs(pcb_t *target,
                                const struct ppap_ptrace_regs *regs) {
  z80_state_t *cpu = (z80_state_t *)target->subsys_data;

  if (!cpu) return -EINVAL;
  if (regs->regset != PPAP_TRACE_REGSET_Z80) return -EINVAL;
  if (regs->words < 18 || regs->words > PPAP_PTRACE_REGS_MAX) return -EINVAL;

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

static int trace_native_contains(const pcb_t *target, uint32_t addr) {
#if defined(__m68k__)
  if (target->user_stack_page) {
    uint32_t base = (uint32_t)(uintptr_t)target->user_stack_page;
    if (addr >= base && addr < base + PAGE_SIZE) return 1;
  }
#elif !defined(__ia16__)
  if (target->stack_page_id != PAGE_ID_INVALID) {
    uint32_t base = (uint32_t)(uintptr_t)mem_region_page_to_ptr(target->stack_page_id);
    if (addr >= base && addr < base + PAGE_SIZE) return 1;
  }
#endif

  if (proc_page_backed_contains(target, addr)) return 1;

  return 0;
}

static int trace_native_read32(const pcb_t *target, uint32_t addr,
                               uint32_t *word) {
  const volatile uint8_t *p0;
  const volatile uint8_t *p1;
  const volatile uint8_t *p2;
  const volatile uint8_t *p3;

  if (!word) return -EINVAL;
  if (addr > UINT32_MAX - 3u) return -EFAULT;
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
  *word = ((uint32_t)*p0 << 24) | ((uint32_t)*p1 << 16) | ((uint32_t)*p2 << 8) |
          (uint32_t)*p3;
#else
  *word = (uint32_t)*p0 | ((uint32_t)*p1 << 8) | ((uint32_t)*p2 << 16) |
          ((uint32_t)*p3 << 24);
#endif
  return 0;
}

static int trace_native_write32(const pcb_t *target, uint32_t addr,
                                uint32_t word) {
  volatile uint8_t *p0;
  volatile uint8_t *p1;
  volatile uint8_t *p2;
  volatile uint8_t *p3;

  if (addr > UINT32_MAX - 3u) return -EFAULT;
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

static int trace_m68k_emu_read32(const pcb_t *target, uint32_t addr,
                                 uint32_t *word) {
  const ppap_m68k_exec_state_t *state =
      (const ppap_m68k_exec_state_t *)target->subsys_data;

  if (!state || !word) return -EINVAL;
  if (addr > state->m68k.mem_size || state->m68k.mem_size - addr < 4)
    return -EFAULT;

  *word = m68k_read32((m68k_state_t *)&state->m68k, addr);
  return 0;
}

static int trace_m68k_emu_write32(const pcb_t *target, uint32_t addr,
                                  uint32_t word) {
  const ppap_m68k_exec_state_t *state =
      (const ppap_m68k_exec_state_t *)target->subsys_data;

  if (!state) return -EINVAL;
  if (addr > state->m68k.mem_size || state->m68k.mem_size - addr < 4)
    return -EFAULT;

  m68k_write32((m68k_state_t *)&state->m68k, addr, word);
  return 0;
}

static int trace_z80_read32(const pcb_t *target, uint32_t addr,
                            uint32_t *word) {
  const z80_state_t *cpu = (const z80_state_t *)target->subsys_data;

  if (!cpu || !word) return -EINVAL;
  if (addr > cpu->mem_size || cpu->mem_size - addr < 4) return -EFAULT;

  *word = (uint32_t)cpu->memory[addr] | ((uint32_t)cpu->memory[addr + 1] << 8) |
          ((uint32_t)cpu->memory[addr + 2] << 16) |
          ((uint32_t)cpu->memory[addr + 3] << 24);
  return 0;
}

static int trace_z80_write32(const pcb_t *target, uint32_t addr,
                             uint32_t word) {
  z80_state_t *cpu = (z80_state_t *)target->subsys_data;

  if (!cpu) return -EINVAL;
  if (addr > cpu->mem_size || cpu->mem_size - addr < 4) return -EFAULT;

  cpu->memory[addr] = (uint8_t)word;
  cpu->memory[addr + 1] = (uint8_t)(word >> 8);
  cpu->memory[addr + 2] = (uint8_t)(word >> 16);
  cpu->memory[addr + 3] = (uint8_t)(word >> 24);
  return 0;
}

static int trace_read32(const pcb_t *target, uint32_t addr, uint32_t *word) {
  if (target->state != PROC_TRACED_STOP) return -EBUSY;

  if (trace_active_surface_for(target) == PPAP_TRACE_SURFACE_ECPU) {
    if (target->subsys == SUBSYS_CPM)
      return trace_z80_read32(target, addr, word);
    if (target->subsys == SUBSYS_PPAP && target->subsys_data)
      return trace_m68k_emu_read32(target, addr, word);
    return -ENOSYS;
  }

  return trace_native_read32(target, addr, word);
}

static int trace_write32(const pcb_t *target, uint32_t addr, uint32_t word) {
  if (target->state != PROC_TRACED_STOP) return -EBUSY;

  if (trace_active_surface_for(target) == PPAP_TRACE_SURFACE_ECPU) {
    if (target->subsys == SUBSYS_CPM)
      return trace_z80_write32(target, addr, word);
    if (target->subsys == SUBSYS_PPAP && target->subsys_data)
      return trace_m68k_emu_write32(target, addr, word);
    return -ENOSYS;
  }

  return trace_native_write32(target, addr, word);
}

static int trace_fill_regs(const pcb_t *target, struct ppap_ptrace_regs *regs) {
  if (target->state != PROC_TRACED_STOP) return -EBUSY;

  if (trace_active_surface_for(target) == PPAP_TRACE_SURFACE_ECPU) {
    if (target->subsys == SUBSYS_CPM) return trace_fill_z80_regs(target, regs);
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

static int trace_store_regs(pcb_t *target,
                            const struct ppap_ptrace_regs *regs) {
  if (!regs) return -EINVAL;
  if (target->state != PROC_TRACED_STOP) return -EBUSY;

  if (trace_active_surface_for(target) == PPAP_TRACE_SURFACE_ECPU) {
    if (target->subsys == SUBSYS_CPM) return trace_store_z80_regs(target, regs);
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

static int trace_fill_caps(const pcb_t *target, struct ppap_ptrace_caps *caps) {
  uint32_t c = PPAP_PTRACE_CAP_GETREGS | PPAP_PTRACE_CAP_SETREGS |
               PPAP_PTRACE_CAP_PEEKPOKE;
  uint32_t surface;
  uint32_t surfaces;
  uint32_t hwbp_slots = 0;

  if (!caps) return -EINVAL;
  if (target->state != PROC_TRACED_STOP) return -EBUSY;

  surface = trace_active_surface_for(target);
  surfaces = trace_surface_mask_for(target);
  if (surface == PPAP_TRACE_SURFACE_ECPU) {
    if (target->subsys == SUBSYS_CPM)
      c |= PPAP_PTRACE_CAP_SINGLESTEP | PPAP_PTRACE_CAP_SW_BP;
    if (target->subsys == SUBSYS_PPAP && target->subsys_data)
      c |= PPAP_PTRACE_CAP_SINGLESTEP | PPAP_PTRACE_CAP_SW_BP;
  }
#if defined(__m68k__)
  if (surface == PPAP_TRACE_SURFACE_REAL) c |= PPAP_PTRACE_CAP_SINGLESTEP;
#endif
  if (surface == PPAP_TRACE_SURFACE_REAL) {
#if defined(__m68k__)
    hwbp_slots = TRACE_HW_BP_MAX;
#elif defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
    hwbp_slots = target_debug_hwbp_slots();
    if (hwbp_slots > TRACE_HW_BP_MAX) hwbp_slots = TRACE_HW_BP_MAX;
#endif
    if (hwbp_slots > 0) c |= PPAP_PTRACE_CAP_HW_BP;
  }

  caps->regset = trace_regset_for(target);
  caps->abi = target->trace_event.abi;
  caps->surface = surface;
  caps->surfaces = surfaces;
  caps->caps = c;
  caps->max_bps = 0;
  if (c & PPAP_PTRACE_CAP_SW_BP) caps->max_bps += TRACE_SW_BP_MAX;
  if (c & PPAP_PTRACE_CAP_HW_BP) caps->max_bps += hwbp_slots;
  return 0;
}

#if defined(__m68k__)
#define M68K_SR_TRACE_BIT 0x8000u

static void trace_m68k_set_trace_bit(pcb_t *target, int enable) {
  uint8_t *exc;
  uint16_t sr;

  if (!target) return;
  exc = (uint8_t *)(uintptr_t)target->sp + 60;
  sr = *(uint16_t *)(void *)exc;
  if (enable)
    sr |= M68K_SR_TRACE_BIT;
  else
    sr &= (uint16_t)~M68K_SR_TRACE_BIT;
  *(uint16_t *)(void *)exc = sr;
}
#else
static void trace_m68k_set_trace_bit(pcb_t *target, int enable) {
  (void)target;
  (void)enable;
}
#endif

static void trace_m68k_update_trace_bit(pcb_t *target) {
#if defined(__m68k__)
  if (trace_active_surface_for(target) != PPAP_TRACE_SURFACE_REAL) {
    trace_m68k_set_trace_bit(target, 0);
    return;
  }
  if (target->trace_step_pending || trace_has_hwbp_for(target))
    trace_m68k_set_trace_bit(target, 1);
  else
    trace_m68k_set_trace_bit(target, 0);
#else
  (void)target;
#endif
}

static int trace_supports_single_step(const pcb_t *target) {
  if (trace_active_surface_for(target) == PPAP_TRACE_SURFACE_ECPU) {
    if (target->subsys == SUBSYS_CPM) return target->subsys_data != 0;
    if (target->subsys == SUBSYS_PPAP && target->subsys_data) return 1;
  }
#if defined(__m68k__)
  if (trace_active_surface_for(target) == PPAP_TRACE_SURFACE_REAL) return 1;
#endif

  return 0;
}

static int trace_request_single_step(pcb_t *target) {
  if (target->state != PROC_TRACED_STOP) return -EBUSY;
  if (!trace_supports_single_step(target)) return -ENOSYS;

  target->trace_step_pending = 1;
  trace_m68k_update_trace_bit(target);
  trace_resume_target(target);
  return 0;
}

#if defined(__m68k__)
int trace_m68k_trace_exception(uint32_t *regs) {
  uint16_t *exc = (uint16_t *)((uint8_t *)regs + 60);
  uint16_t sr = exc[0];
  uint32_t pc = ((uint32_t)exc[1] << 16) | exc[2];

  /* Always clear T-bit so the handler is one-shot unless re-armed. */
  exc[0] = (uint16_t)(sr & (uint16_t)~M68K_SR_TRACE_BIT);

  if (!current->tracer_pid) return 0;
  if (current->state != PROC_RUNNABLE) return 0;
  if (trace_active_surface_for(current) != PPAP_TRACE_SURFACE_REAL) return 0;
  if (current->trace_step_pending) {
    current->trace_step_pending = 0;
    trace_debug_stop(PPAP_TRACE_ABI_PPAP, pc, PPAP_DEBUG_STOP_STEP);
    return 1;
  }
  if (!trace_has_hwbp_for(current)) return 0;
  if (trace_hwbp_hit(current, pc)) {
    trace_debug_stop(PPAP_TRACE_ABI_PPAP, pc, PPAP_DEBUG_STOP_HW_BP);
    return 1;
  }
  trace_m68k_set_trace_bit(current, 1);
  return 0;
}
#endif

static void trace_clear_swbp(pcb_t *target) {
  target->trace_swbp_skip_once = 0;
  target->trace_swbp_skip_pc = 0;
  for (uint32_t i = 0; i < TRACE_SW_BP_MAX; i++) {
    target->trace_swbp[i].addr = 0;
    target->trace_swbp[i].used = 0;
    target->trace_swbp[i].enabled = 0;
  }
}

static void trace_clear_hwbp(pcb_t *target) {
  for (uint32_t i = 0; i < TRACE_HW_BP_MAX; i++) {
    target->trace_hwbp[i].addr = 0;
    target->trace_hwbp[i].used = 0;
    target->trace_hwbp[i].enabled = 0;
  }
}

static void trace_clear_breakpoints(pcb_t *target) {
  trace_clear_swbp(target);
  trace_clear_hwbp(target);
}

static int trace_supports_swbp(const pcb_t *target) {
  if (trace_active_surface_for(target) != PPAP_TRACE_SURFACE_ECPU) return 0;
  if (target->subsys == SUBSYS_CPM) return target->subsys_data != 0;
  if (target->subsys == SUBSYS_PPAP && target->subsys_data) return 1;
  return 0;
}

static int trace_supports_hwbp(const pcb_t *target) {
#if defined(__m68k__)
  if (trace_active_surface_for(target) == PPAP_TRACE_SURFACE_REAL) return 1;
#elif defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
  if (trace_active_surface_for(target) == PPAP_TRACE_SURFACE_REAL) {
    uint32_t slots = target_debug_hwbp_slots();
    return slots > 0;
  }
#else
  (void)target;
#endif
  return 0;
}

#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
#define ARM_SCB_HFSR_ADDR (0xE000ED2Cu)
#define ARM_SCB_DFSR_ADDR (0xE000ED30u)
#define ARM_SCB_HFSR_DEBUGEVT (1u << 31)
#define ARM_SCB_DFSR_BKPT (1u << 1)

static uint32_t trace_arm_hwbp_slots(void) {
  uint32_t slots = target_debug_hwbp_slots();
  if (slots > TRACE_HW_BP_MAX) slots = TRACE_HW_BP_MAX;
  return slots;
}

static void trace_arm_hwbp_sync_target(const pcb_t *target) {
  uint32_t slots = trace_arm_hwbp_slots();

  for (uint32_t i = 0; i < slots; i++) {
    int use_slot = 0;
    uint32_t addr = 0;

    if (target && target->tracer_pid &&
        trace_active_surface_for(target) == PPAP_TRACE_SURFACE_REAL &&
        target->trace_hwbp[i].used && target->trace_hwbp[i].enabled) {
      use_slot = 1;
      addr = target->trace_hwbp[i].addr;
    }

    if (use_slot) {
      if (target_debug_hwbp_set(i, addr) < 0) target_debug_hwbp_clear(i);
    } else {
      target_debug_hwbp_clear(i);
    }
  }
}
#endif

static int __attribute__((unused)) trace_has_hwbp_for(const pcb_t *target) {
  if (!trace_supports_hwbp(target)) return 0;
  for (uint32_t i = 0; i < TRACE_HW_BP_MAX; i++) {
    if (target->trace_hwbp[i].used && target->trace_hwbp[i].enabled) return 1;
  }
  return 0;
}

#if defined(__m68k__) || defined(__ARM_ARCH) || defined(__arm__) || \
    defined(__thumb__)
static int trace_hwbp_hit(const pcb_t *target, uint32_t pc) {
  if (!trace_supports_hwbp(target)) return 0;
  for (uint32_t i = 0; i < TRACE_HW_BP_MAX; i++) {
    if (!target->trace_hwbp[i].used || !target->trace_hwbp[i].enabled) continue;
    if (target->trace_hwbp[i].addr == pc) return 1;
  }
  return 0;
}
#endif

int trace_has_swbp(void) {
  if (trace_active_surface_for(current) != PPAP_TRACE_SURFACE_ECPU) return 0;
  for (uint32_t i = 0; i < TRACE_SW_BP_MAX; i++) {
    if (current->trace_swbp[i].used && current->trace_swbp[i].enabled) return 1;
  }
  return 0;
}

static int trace_set_swbp(pcb_t *target, struct ppap_ptrace_bp *bp) {
  if (!bp) return -EINVAL;
  if (target->state != PROC_TRACED_STOP) return -EBUSY;
  if (!trace_supports_swbp(target)) return -ENOSYS;
  if (bp->flags != 0 && bp->flags != PPAP_PTRACE_BP_SW) return -EINVAL;

  for (int32_t i = 0; i < TRACE_SW_BP_MAX; i++) {
    if (target->trace_swbp[i].used && target->trace_swbp[i].enabled &&
        target->trace_swbp[i].addr == bp->addr) {
      bp->id = i;
      bp->flags = PPAP_PTRACE_BP_SW;
      return 0;
    }
  }

  for (int32_t i = 0; i < TRACE_SW_BP_MAX; i++) {
    if (target->trace_swbp[i].used) continue;
    target->trace_swbp[i].addr = bp->addr;
    target->trace_swbp[i].used = 1;
    target->trace_swbp[i].enabled = 1;
    bp->id = i;
    bp->flags = PPAP_PTRACE_BP_SW;
    return 0;
  }

  return -ENOSPC;
}

static int trace_set_hwbp(pcb_t *target, struct ppap_ptrace_bp *bp) {
  if (!bp) return -EINVAL;
  if (target->state != PROC_TRACED_STOP) return -EBUSY;
  if (!trace_supports_hwbp(target)) return -ENOSYS;
  if (bp->flags != 0 && bp->flags != PPAP_PTRACE_BP_HW) return -EINVAL;

  for (int32_t i = 0; i < TRACE_HW_BP_MAX; i++) {
    if (target->trace_hwbp[i].used && target->trace_hwbp[i].enabled &&
        target->trace_hwbp[i].addr == bp->addr) {
      bp->id = TRACE_SW_BP_MAX + i;
      bp->flags = PPAP_PTRACE_BP_HW;
      return 0;
    }
  }

  for (int32_t i = 0; i < TRACE_HW_BP_MAX; i++) {
    if (target->trace_hwbp[i].used) continue;
    target->trace_hwbp[i].addr = bp->addr;
    target->trace_hwbp[i].used = 1;
    target->trace_hwbp[i].enabled = 1;
    bp->id = TRACE_SW_BP_MAX + i;
    bp->flags = PPAP_PTRACE_BP_HW;
    return 0;
  }

  return -ENOSPC;
}

static int trace_clr_swbp(pcb_t *target, struct ppap_ptrace_bp *bp) {
  int32_t id;

  if (!bp) return -EINVAL;
  if (target->state != PROC_TRACED_STOP) return -EBUSY;
  if (!trace_supports_swbp(target)) return -ENOSYS;

  id = bp->id;
  if (id < 0 || id >= TRACE_SW_BP_MAX) return -EINVAL;
  if (!target->trace_swbp[id].used) return -EINVAL;

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

static int trace_clr_hwbp(pcb_t *target, struct ppap_ptrace_bp *bp) {
  int32_t id;

  if (!bp) return -EINVAL;
  if (target->state != PROC_TRACED_STOP) return -EBUSY;
  if (!trace_supports_hwbp(target)) return -ENOSYS;

  id = bp->id - TRACE_SW_BP_MAX;
  if (id < 0 || id >= TRACE_HW_BP_MAX) return -EINVAL;
  if (!target->trace_hwbp[id].used) return -EINVAL;

  target->trace_hwbp[id].addr = 0;
  target->trace_hwbp[id].used = 0;
  target->trace_hwbp[id].enabled = 0;
  return 0;
}

static int trace_set_bp(pcb_t *target, struct ppap_ptrace_bp *bp) {
  if (!bp) return -EINVAL;
  if (bp->flags == 0) {
    if (trace_supports_swbp(target)) return trace_set_swbp(target, bp);
    if (trace_supports_hwbp(target)) return trace_set_hwbp(target, bp);
    return -ENOSYS;
  }
  if (bp->flags == PPAP_PTRACE_BP_SW) return trace_set_swbp(target, bp);
  if (bp->flags == PPAP_PTRACE_BP_HW) return trace_set_hwbp(target, bp);
  return -EINVAL;
}

static int trace_clr_bp(pcb_t *target, struct ppap_ptrace_bp *bp) {
  if (!bp) return -EINVAL;
  if (bp->id >= TRACE_SW_BP_MAX) return trace_clr_hwbp(target, bp);
  return trace_clr_swbp(target, bp);
}

int trace_before_syscall(uint32_t *frame, uint32_t nr, uint32_t a4,
                         uint32_t a5) {
  if (!(current->trace_mode & PPAP_TRACE_MODE_PPAP_SYSCALL)) return 0;
  if (current->trace_syscall_phase != TRACE_PHASE_ENTER) return 0;
  if (nr == SYS_PTRACE) return 0;

  current->trace_syscall_phase = TRACE_PHASE_EXIT;
  trace_fill_event(PPAP_TRACE_EVENT_SYSCALL_ENTER, PPAP_TRACE_ABI_PPAP, nr,
                   frame[0], frame[1], frame[2], frame[3], a4, a5, 0, 0);
  trace_stop_current(1);
  return 1;
}

void trace_after_syscall(uint32_t *frame, uint32_t nr, uint32_t a4, uint32_t a5,
                         long ret) {
  if (!(current->trace_mode & PPAP_TRACE_MODE_PPAP_SYSCALL)) return;
  if (current->trace_syscall_phase != TRACE_PHASE_EXIT) return;
  if (current->state != PROC_RUNNABLE) return;
  if (nr == SYS_PTRACE) return;

  current->trace_syscall_phase = TRACE_PHASE_ENTER;
  trace_fill_event(PPAP_TRACE_EVENT_SYSCALL_EXIT, PPAP_TRACE_ABI_PPAP, nr,
                   frame[0], frame[1], frame[2], frame[3], a4, a5, (int32_t)ret,
                   0);
  trace_stop_current(0);
}

int trace_before_subsys(uint32_t abi, uint32_t nr, uint32_t a0, uint32_t a1,
                        uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
  if (!(current->trace_mode & PPAP_TRACE_MODE_SUBSYS_CALL)) return 0;
  if (current->trace_subsys_phase != TRACE_PHASE_ENTER) return 0;

  current->trace_subsys_phase = TRACE_PHASE_EXIT;
  trace_fill_event(PPAP_TRACE_EVENT_SUBSYS_ENTER, abi, nr, a0, a1, a2, a3, a4,
                   a5, 0, 0);
  trace_stop_current(0);
  return 1;
}

void trace_after_subsys(uint32_t abi, uint32_t nr, uint32_t a0, uint32_t a1,
                        uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5,
                        int32_t ret) {
  if (!(current->trace_mode & PPAP_TRACE_MODE_SUBSYS_CALL)) return;
  if (current->trace_subsys_phase != TRACE_PHASE_EXIT) return;
  if (current->state != PROC_RUNNABLE) return;

  current->trace_subsys_phase = TRACE_PHASE_ENTER;
  trace_fill_event(PPAP_TRACE_EVENT_SUBSYS_EXIT, abi, nr, a0, a1, a2, a3, a4,
                   a5, ret, 0);
  trace_stop_current(0);
}

void trace_debug_stop(uint32_t abi, uint32_t pc, uint32_t flags) {
  trace_fill_event(PPAP_TRACE_EVENT_DEBUG_STOP, abi, 0, pc, 0, 0, 0, 0, 0, 0,
                   flags);
  trace_stop_current(0);
}

int trace_maybe_stop_at_swbp(uint32_t abi, uint32_t pc) {
  if (!current->tracer_pid) return 0;
  if (current->state != PROC_RUNNABLE) return 0;
  if (trace_active_surface_for(current) != PPAP_TRACE_SURFACE_ECPU) return 0;

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
    if (current->trace_swbp[i].addr != pc) continue;
    current->trace_swbp_skip_once = 1;
    current->trace_swbp_skip_pc = pc;
    trace_debug_stop(abi, pc, PPAP_DEBUG_STOP_SW_BP);
    return 1;
  }
  return 0;
}

void trace_arm_hwbp_on_switch(const pcb_t *next) {
#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
  trace_arm_hwbp_sync_target(next);
#else
  (void)next;
#endif
}

int trace_arm_hardfault_debug_stop(uint32_t *psp_frame) {
#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
  volatile uint32_t *const hfsr = (volatile uint32_t *)ARM_SCB_HFSR_ADDR;
  volatile uint32_t *const dfsr = (volatile uint32_t *)ARM_SCB_DFSR_ADDR;
  uint32_t hfsr_bits;
  uint32_t dfsr_bits;
  uint32_t pc;

  if (!psp_frame) return 0;
  if (!current || !current->tracer_pid) return 0;
  if (current->state != PROC_RUNNABLE) return 0;
  if (trace_active_surface_for(current) != PPAP_TRACE_SURFACE_REAL) return 0;
  if (!trace_has_hwbp_for(current)) return 0;

  hfsr_bits = *hfsr;
  dfsr_bits = *dfsr;
  if ((hfsr_bits & ARM_SCB_HFSR_DEBUGEVT) == 0 &&
      (dfsr_bits & ARM_SCB_DFSR_BKPT) == 0)
    return 0;

  pc = psp_frame[6] & ~1u;
  if (!trace_hwbp_hit(current, pc)) return 0;

  if (hfsr_bits & ARM_SCB_HFSR_DEBUGEVT) *hfsr = ARM_SCB_HFSR_DEBUGEVT;
  if (dfsr_bits) *dfsr = dfsr_bits;

  trace_debug_stop(PPAP_TRACE_ABI_PPAP, pc, PPAP_DEBUG_STOP_HW_BP);
  return 1;
#else
  (void)psp_frame;
  return 0;
#endif
}

void trace_exec_stop(void) {
  if (!current->trace_requested) return;

  current->trace_requested = 0;
  current->trace_syscall_phase = TRACE_PHASE_ENTER;
  current->trace_subsys_phase = TRACE_PHASE_ENTER;
  current->trace_step_pending = 0;
  current->trace_surface = (uint8_t)trace_default_surface_for(current);
  trace_clear_breakpoints(current);
  trace_fill_event(PPAP_TRACE_EVENT_EXEC, PPAP_TRACE_ABI_PPAP, SYS_EXECVE, 0, 0,
                   0, 0, 0, 0, 0, 0);
  trace_stop_current(0);
}

#if defined(__ia16__)
/* Stub — ptrace not supported on i16 (saves ~4 KB text) */
long sys_ptrace(long req, long pid, void *addr, void *data) {
  (void)req; (void)pid; (void)addr; (void)data;
  return -(long)ENOSYS;
}
#else
long sys_ptrace(long req, long pid, void *addr, void *data) {
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
    trace_clear_breakpoints(current);
    __builtin_memset(&current->trace_event, 0, sizeof(current->trace_event));
    return 0;
  }

  if (req == PTRACE_ATTACH) {
    pcb_t *target = trace_find_process(pid);
    if (!target || target->state == PROC_ZOMBIE) return -(long)ESRCH;
    if (target == current) return -(long)EPERM;
    if (target->tracer_pid != 0 || target->trace_requested) return -(long)EPERM;

    target->tracer_pid = current->pid;
    target->trace_requested = 0;
    trace_reset_mode_state(target, 0);
    target->trace_surface = (uint8_t)trace_default_surface_for(target);
    target->trace_wait_pending = 1;
    target->trace_step_pending = 0;
    trace_clear_breakpoints(target);
    __builtin_memset(&target->trace_event, 0, sizeof(target->trace_event));
    target->trace_event.event = PPAP_TRACE_EVENT_DEBUG_STOP;
    target->trace_event.abi = PPAP_TRACE_ABI_PPAP;
    target->state = PROC_TRACED_STOP;
    trace_wake_tracer(target);
    return 0;
  }

  pcb_t *target = trace_find_tracee(current->pid, pid);
  if (!target) return -(long)ESRCH;

  switch (req) {
    case PTRACE_GETEVENT:
      if (!data) return -(long)EINVAL;
      *(struct ppap_ptrace_event *)data = target->trace_event;
      return 0;
    case PTRACE_PEEKDATA:
      if (!data) return -(long)EINVAL;
      return trace_read32(target, (uint32_t)(uintptr_t)addr, (uint32_t *)data);
    case PTRACE_POKEDATA:
      if (!data) return -(long)EINVAL;
      return trace_write32(target, (uint32_t)(uintptr_t)addr,
                           *(const uint32_t *)data);
    case PTRACE_GETREGS:
      if (!data) return -(long)EINVAL;
      return trace_fill_regs(target, (struct ppap_ptrace_regs *)data);
    case PTRACE_SETREGS:
      if (!data) return -(long)EINVAL;
      {
        int rc =
            trace_store_regs(target, (const struct ppap_ptrace_regs *)data);
        if (rc == 0) target->trace_swbp_skip_once = 0;
        return rc;
      }
    case PTRACE_GETCAPS:
      if (!data) return -(long)EINVAL;
      return trace_fill_caps(target, (struct ppap_ptrace_caps *)data);
    case PTRACE_GETSURFACE:
      if (!data) return -(long)EINVAL;
      *(uint32_t *)data = trace_active_surface_for(target);
      return 0;
    case PTRACE_SETSURFACE:
      return trace_set_surface(target, (uint32_t)(uintptr_t)addr);
    case PTRACE_SETMODE: {
      uint8_t mode = (uint8_t)(uintptr_t)addr;
      if (mode & (uint8_t)~TRACE_MODE_MASK) return -(long)EINVAL;
      trace_reset_mode_state(target, mode);
      return 0;
    }
    case PTRACE_SETBP:
      if (!data) return -(long)EINVAL;
      return trace_set_bp(target, (struct ppap_ptrace_bp *)data);
    case PTRACE_CLRBP:
      if (!data) return -(long)EINVAL;
      return trace_clr_bp(target, (struct ppap_ptrace_bp *)data);
    case PTRACE_SINGLESTEP:
      return trace_request_single_step(target);
    case PTRACE_CONT:
      target->trace_step_pending = 0;
      trace_m68k_update_trace_bit(target);
      trace_resume_target(target);
      return 0;
    case PTRACE_SYSCALL:
      target->trace_step_pending = 0;
      trace_m68k_update_trace_bit(target);
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
      trace_m68k_set_trace_bit(target, 0);
      trace_clear_breakpoints(target);
      __builtin_memset(&target->trace_event, 0, sizeof(target->trace_event));
      trace_resume_target(target);
      return 0;
    default:
      return -(long)EINVAL;
  }
}

#endif /* !__ia16__ (ptrace) */

/* ── sys_exit ─────────────────────────────────────────────────────────────────
 */

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
 *   - tracked page-backed slots are released here in sys_exit() (step 3).
 *   - stack_page is freed later in sys_waitpid() when the parent reaps
 *     the zombie.  Orphans are reparented to init (step 6), ensuring
 *     init can always reap them.
 *
 * Note: sys_exit runs inside SVC_Handler (Handler mode).  sched_yield()
 * only pends PendSV, which tail-chains after SVC returns.  There is no
 * need for an infinite loop — the ZOMBIE process will never be scheduled
 * again because sched_next() only picks PROC_RUNNABLE processes.
 */
long sys_exit(long status) {
  /* Guard against double-exit.  musl's _Exit() calls SYS_exit_group
   * then SYS_exit in a loop.  On RISC-V, the second call can reach
   * here if the context switch after the first exit doesn't happen
   * before the ecall return path re-executes. */
  if (current->state == PROC_ZOMBIE) {
#if defined(__xtensa__)
    /* On Xtensa, syscall handler performs the post-syscall switch when it
     * sees !PROC_RUNNABLE. Triggering a nested cooperative yield here can
     * bounce back to user stub loops. */
    return 0;
#else
    sched_yield();
    return 0; /* unreachable — zombie won't be scheduled */
#endif
  }

  current->exit_status = (int)status;

  /* Let the subsystem clean up while fds are still open
   * (e.g. restore terminal state). */
  if (current->subsys < SUBSYS_MAX) {
    const subsys_ops_t *ops = subsys_ops_table[current->subsys];
    if (ops && ops->on_exit) ops->on_exit(current);
  }

  /* Close all open fds */
  for (int _i = 0; _i < FD_MAX; _i++) {
    if (current->fd_map[_i] != FD_DESC_NONE) {
      mod_vfs.fd_release(current->fd_map[_i]);
      current->fd_map[_i] = FD_DESC_NONE;
    }
  }

  /* Free user pages only if we own them (vfork_parent == NULL means
   * either this isn't a vfork child, or execve already replaced them) */
  if (!current->vfork_parent) {
    image_release_owned_segments(&current->image);
#if !defined(__ia16__)
    if (current->stack_page_id != PAGE_ID_INVALID &&
        proc_page_backed_contains(current, (uintptr_t)mem_region_page_to_ptr(current->stack_page_id)))
      current->stack_page_id = PAGE_ID_INVALID;
#endif
    proc_release_tracked_pages(current, 0, USER_PAGES_MAX);
#if defined(__m68k__)
    if (current->user_stack_page) {
      proc_release_stack_page(&current->user_stack_page);
    }
#endif
  } else {
    /* vfork child exiting without exec: free child-owned pages only
     * (e.g. the user stack copy allocated by sys_vfork). */
    proc_release_private_tracked_pages(current, current->vfork_parent);
    /* Clear stack_page if it's the parent's — waitpid frees stack_page,
     * and freeing the parent's stack would corrupt its kernel stack.
     * This can happen when execve fails in a vfork child: sys_execve
     * restores the parent's stack_page into the child's pcb. */
    if (current->stack_page_id == current->vfork_parent->stack_page_id)
      current->stack_page_id = PAGE_ID_INVALID;
#if defined(__m68k__)
    /* Free child's user stack copy if different from parent's */
    if (current->user_stack_page &&
        current->user_stack_page != current->vfork_parent->user_stack_page) {
      proc_release_stack_page(&current->user_stack_page);
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
    if (child->state == PROC_FREE || child->ppid != current->pid) continue;
    child->ppid = 1;
    if (child->state == PROC_ZOMBIE) {
      for (uint32_t j = 0; j < PROC_MAX; j++) {
        if (proc_table[j].pid == 1 && proc_table[j].state == PROC_BLOCKED) {
          proc_table[j].state = PROC_RUNNABLE;
          break;
        }
      }
    }
  }

  current->state = PROC_ZOMBIE;
#if defined(__xtensa__)
  /* Xtensa: return to syscall handler and let it switch away based on
   * current->state != PROC_RUNNABLE. */
  return 0;
#else
  sched_yield();
  return 0; /* never reached — PendSV switches away after SVC returns */
#endif
}

/* ── sys_getpid ───────────────────────────────────────────────────────────────
 */

long sys_getpid(void) { return (long)current->pid; }

/* ── sys_vfork ────────────────────────────────────────────────────────────────
 */

/*
 * Create a child process.  The parent is blocked until the child calls
 * execve() or _exit().
 *
 * The child gets its own stack page with a copy of the parent's exception
 * frame (return register = 0 for child). The child shares the parent's
 * tracked page-backed slots (GOT/data).
 *
 * frame: pointer to the parent's stacked exception frame
 *   ARM:  [r0, r1, r2, r3, r12, lr, pc, xpsr] on PSP
 *   m68k: &regs[1] (d1 slot) in the TRAP #0 saved register frame
 */
long sys_vfork(uint32_t *frame) {
  /* 1. Allocate child PCB */
  pcb_t *child = proc_alloc();
  if (!child) return -(long)ENOMEM;

  /* 2. Allocate stack page for child */
  proc_image_segment_t stack_region = {0};
  if (mem_region_alloc(&stack_region, PPAP_MEM_RAM_STACK, PAGE_SIZE,
                       PROC_IMAGE_SEG_WRITABLE) < 0) {
    proc_free(child);
    return -(long)ENOMEM;
  }
  void *stack = stack_region.base;
  child->stack_page_id = mem_region_ptr_to_page(stack);

  /* 3. Share parent's user_pages with child */
  proc_copy_page_tracking(child, current);

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
#if defined(__ia16__)
  /* TODO: i16 vfork — use mem_region_page_read for cross-segment copy */
  mem_region_page_read(current->stack_page_id, 0, stack, PAGE_SIZE);
  uintptr_t frame_off = 0;
  uint32_t *child_frame = (uint32_t *)stack;
#else
  memcpy(stack, mem_region_page_to_ptr(current->stack_page_id), PAGE_SIZE);

  /* Calculate child's frame position at the same offset as parent's */
  uintptr_t frame_off = (uintptr_t)frame - (uintptr_t)mem_region_page_to_ptr(current->stack_page_id);
  uint32_t *child_frame = (uint32_t *)((uint8_t *)stack + frame_off);
#endif

#if defined(__m68k__)
  /* m68k: The TRAP #0 frame (15 regs + SR + PC) has the same layout as
   * the switch.S context frame.  The child returns directly to user code
   * via switch.S restore (movem.l + rte), bypassing TRAP #0 cleanup.
   *
   * frame = &regs[1] (d1 slot).  child_frame - 1 = d0 slot. */
  uint32_t *child_regs = child_frame - 1; /* d0 slot */
  child_regs[0] = 0;                      /* d0 = 0 (child return) */
  child_regs[13] = current->got_base;     /* a5 = GOT base for PIC */

  child->sp = (uint32_t)(uintptr_t)child_regs;

  /* m68k user mode: USP points to a user_stack_page (separate from
   * stack_page / SSP).  The child must have its own user stack copy. */
  {
    void *parent_ustack = current->user_stack_page;
    if (parent_ustack) {
      void *child_ustack = NULL;
      uint32_t child_usp = 0;
      if (vfork_copy_user_stack(parent_ustack, current->usp,
                                &child_ustack, &child_usp) < 0) {
        proc_release_stack_page(&stack);
        child->stack_page_id = PAGE_ID_INVALID;
        proc_free(child);
        return -(long)ENOMEM;
      }
      child->user_stack_page = child_ustack;
      child->usp = child_usp;

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
#elif defined(__riscv)
  /* RISC-V: The ecall trap frame (36 words) is on the child's kernel
   * stack (copied from parent's kernel stack).  child_frame points to
   * saved a0 in the trap frame (sp + 32).
   *
   * Set child's a0 = 0 (vfork return value for child). */
  child_frame[0] = 0;

  /* child->sp = trap frame base on the child's kernel stack */
  child->sp = (uint32_t)(uintptr_t)(child_frame - 8);
  child->kernel_sp = (uint32_t)(uintptr_t)stack + PAGE_SIZE;

  /* RISC-V mscratch split: the child must have its own user stack copy. */
  {
    uint32_t ustack_slot = USER_PAGES_MAX - 1;
    page_id_t parent_ustack_id = current->user_pages[ustack_slot];
    if (parent_ustack_id != PAGE_ID_INVALID) {
      void *parent_ustack = mem_region_page_to_ptr(parent_ustack_id);
      void *child_ustack = NULL;
      uint32_t *child_tf = child_frame - 8; /* trap frame base */
      uint32_t child_usp = 0;
      if (vfork_copy_user_stack(parent_ustack, child_tf[32],
                                &child_ustack, &child_usp) < 0) {
        proc_release_stack_page(&stack);
        child->stack_page_id = PAGE_ID_INVALID;
        proc_free(child);
        return -(long)ENOMEM;
      }
      child->user_pages[ustack_slot] = mem_region_ptr_to_page(child_ustack);
      child_tf[32] = child_usp; /* patch TF_USER_SP */
    }
  }

#elif defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
  /* ARM: Set child's r0 = 0 (child sees vfork return 0) */
  child_frame[0] = 0;

  /* Build SW callee-saved frame below the HW frame.
   * r9 = GOT base so PIC addressing works after PendSV restore.
   *
   * ARMv8-M (Cortex-M33): SW frame is 9 words (r4-r11 + EXC_RETURN).
   * EXC_RETURN bit 4 = 1 (no FPU frame) — child starts with clean FPU.
   * ARMv6-M (Cortex-M0+): SW frame is 8 words (r4-r11). */
#if __ARM_ARCH >= 8
  uint32_t *sw = child_frame - 9;
  memset(sw, 0, 9 * sizeof(uint32_t));
  sw[5] = current->got_base; /* r9 = GOT SRAM address for PIC */
  sw[8] = 0xFFFFFFFDu;       /* EXC_RETURN: Thread/PSP, no FPU frame */
#else
  uint32_t *sw = child_frame - 8;
  memset(sw, 0, 8 * sizeof(uint32_t));
  sw[5] = current->got_base; /* r9 = GOT SRAM address for PIC */
#endif

  child->sp = (uint32_t)(uintptr_t)sw;

#elif defined(__xtensa__)
  /* Xtensa: Build a new-process frame for the child.
   *
   * XtExcFrame layout: exit, pc, ps, a0, a1, a2, ...
   * 'frame' points to a2, so frame[-4] = pc, frame[-1] = a1.
   *
   * The child resumes at the instruction after the ILL trap (pc already +3)
   * with a2 = 0 (switch.S .Lnew_process clears a2 before jx).
   *
   * Xtensa shares user/kernel on one stack page.  Allocate a separate
   * user stack page via the shared vfork_copy_user_stack helper so the
   * child's pre-execve writes don't corrupt the parent's saved frames. */
  {
    uint32_t child_pc = frame[-4];      /* pc (already advanced +3) */
    uint32_t child_user_sp = frame[-1]; /* a1 = user SP at syscall */
    uint32_t child_a0 = frame[-2];      /* a0 = return addr to caller */
    uint32_t child_a3 = frame[1];       /* a3 (compiler expects preserved) */

    void *child_ustack = NULL;
    uint32_t remapped_sp = 0;
    if (vfork_copy_user_stack(mem_region_page_to_ptr(current->stack_page_id), child_user_sp,
                              &child_ustack, &remapped_sp) < 0) {
      proc_release_stack_page(&stack);
      child->stack_page_id = PAGE_ID_INVALID;
      proc_free(child);
      return -(long)ENOMEM;
    }
    child_user_sp = remapped_sp;

    /* Remap a3 if it points into the parent's stack page */
    {
      uint32_t pbase = (uint32_t)(uintptr_t)mem_region_page_to_ptr(current->stack_page_id);
      uint32_t cbase = (uint32_t)(uintptr_t)child_ustack;
      if (child_a3 >= pbase && child_a3 < pbase + PAGE_SIZE)
        child_a3 = cbase + (child_a3 - pbase);
    }

    /* Build new-process frame at top of child's kernel stack page. */
    uint32_t *sp = (uint32_t *)((uint8_t *)stack + PAGE_SIZE);
    sp = (uint32_t *)((uintptr_t)sp & ~0xFu);
    *--sp = child_a3;                   /* [SP+36] a3 = preserved reg */
    *--sp = child_a0;                   /* [SP+32] a0 = return addr */
    *--sp = child_user_sp;              /* [SP+28] user SP */
    *--sp = (1u << 5);                  /* [SP+24] PS: UM=1 */
    *--sp = child_pc;                   /* [SP+20] entry */
    *--sp = 1u;                         /* [SP+16] exit = 1 */
    *--sp = 0;                          /* [SP+12] ABI scratch */
    *--sp = 0;                          /* [SP+8]  ABI scratch */
    *--sp = 0;                          /* [SP+4]  ABI scratch */
    *--sp = 0;                          /* [SP+0]  ABI scratch */
    child->sp = (uint32_t)(uintptr_t)sp;
  }

#elif defined(__ia16__)
  /* TODO: i16 vfork frame setup for P-4 */
  (void)child_frame;
  child->sp = (uint32_t)(uintptr_t)((uint8_t *)stack + frame_off);
#else
#error "sys_vfork: unsupported architecture — add child frame setup"
#endif
  child->ticks_remaining = PROC_DEFAULT_TICKS;

  /* 5. Set up identity and relationships */
  child->ppid = current->pid;
  child->pgid = current->pgid; /* inherit process group (like real fork) */
  child->sid = current->sid;   /* inherit session ID */
  child->vfork_parent = current;
  child->got_base = current->got_base;
  child->tp_value = current->tp_value;
  memcpy(child->cwd, current->cwd, sizeof(child->cwd));
  memcpy(child->comm, current->comm, sizeof(child->comm));

  /* 6. Inherit file descriptors from parent */
  for (int _i = 0; _i < FD_MAX; _i++) {
    child->fd_map[_i] = current->fd_map[_i];
    if (current->fd_map[_i] != FD_DESC_NONE)
      mod_vfs.fd_acquire(current->fd_map[_i]);
  }

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
  svc_restart[core_id()] = 0;

  return (long)child->pid;
}

/* ── sys_waitpid ──────────────────────────────────────────────────────────────
 */

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
long sys_waitpid(long pid, long status_ptr, long options) {
  pcb_t *zombie = NULL;
  pcb_t *stopped = NULL;
  int has_match = 0;
  int zombie_is_child = 0;

  /* Scan for matching child or traced process. */
  for (uint32_t i = 1; i < PROC_MAX; i++) {
    pcb_t *p = &proc_table[i];
    int is_child;
    int is_tracee;

    if (p->state == PROC_FREE) continue;
    is_child = (p->ppid == current->pid);
    is_tracee = (p->tracer_pid == current->pid);
    if (!is_child && !is_tracee) continue;
    if (pid > 0 && p->pid != (pid_t)pid) continue;

    has_match = 1;

    if ((options & WSTOPPED) && p->state == PROC_TRACED_STOP &&
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
      int status = W_STOPCODE(SIGTRAP);
      if (sys_copy_to_user((uintptr_t)status_ptr, &status, sizeof(status)) < 0)
        return -(long)EFAULT;
    }
    stopped->trace_wait_pending = 0;
    return (long)stopped->pid;
  }

  if (zombie) {
    pid_t cpid = zombie->pid;

    if (status_ptr) {
      int status = W_EXITCODE(zombie->exit_status);
      if (sys_copy_to_user((uintptr_t)status_ptr, &status, sizeof(status)) < 0)
        return -(long)EFAULT;
    }

    if (!zombie_is_child) {
      /* Non-child tracees report exit but are reaped by their parent. */
      zombie->tracer_pid = 0;
      zombie->trace_requested = 0;
      trace_reset_mode_state(zombie, 0);
      zombie->trace_surface = (uint8_t)trace_default_surface_for(zombie);
      zombie->trace_wait_pending = 0;
      zombie->trace_step_pending = 0;
      trace_clear_breakpoints(zombie);
      __builtin_memset(&zombie->trace_event, 0, sizeof(zombie->trace_event));
      return (long)cpid;
    }

    /* Reap the zombie child. */
    /* Free zombie's stack page */
    if (zombie->stack_page_id != PAGE_ID_INVALID) {
#if defined(__ia16__)
      mem_region_page_free(zombie->stack_page_id);
#else
      { void *zs = mem_region_page_to_ptr(zombie->stack_page_id); proc_release_stack_page(&zs); }
#endif
      zombie->stack_page_id = PAGE_ID_INVALID;
    }

    proc_free(zombie);
    return (long)cpid;
  }

  if (!has_match) return -(long)ECHILD;

  if (options & WNOHANG) return 0;

  /* Block and arrange for syscall restart.
   * sys_exit will wake us by setting PROC_RUNNABLE.
   * SVC_Handler will restore frame[0] and PC-2 so the SVC re-executes. */
  current->state = PROC_BLOCKED;
  svc_set_restart();
  sched_yield();
  return 0; /* value ignored — SVC_Handler restores original frame[0] */
}

/* ── sys_execve ───────────────────────────────────────────────────────────────
 */

/*
 * Replace the current process image with a new ELF binary.
 * Called from user space (typically after vfork).
 *
 * On success: never returns — the new program starts executing.
 * On failure: returns negative errno.
 */
#define EXEC_ARG_BYTES_MAX 1024u

static int sys_execve_copy_user_argv(const char **argv_out,
                                     char *arg_buf,
                                     size_t arg_buf_size,
                                     uintptr_t argv_ptr) {
  size_t used = 0;

  if (argv_ptr == 0u) return 0;

  for (int i = 0; i < EXEC_ARGV_MAX; i++) {
    uintptr_t arg_ptr;
    int rc = sys_copy_from_user(&arg_ptr,
                                argv_ptr + (uintptr_t)(i * sizeof(arg_ptr)),
                                sizeof(arg_ptr));
    size_t len;

    if (rc < 0) return rc;
    if (arg_ptr == 0u) {
      argv_out[i] = NULL;
      return 0;
    }
    rc = sys_copy_user_string(arg_buf + used, arg_buf_size - used, arg_ptr);
    if (rc < 0) return (rc == -(long)ENAMETOOLONG) ? -(long)E2BIG : rc;
    argv_out[i] = arg_buf + used;
    len = __builtin_strlen(argv_out[i]) + 1;
    used += len;
    if (used >= arg_buf_size) return -(long)E2BIG;
  }
  return -(long)E2BIG;
}

long sys_execve(uintptr_t path_ptr, uintptr_t argv_ptr) {
  char path[VFS_PATH_MAX];
  const char *argv_copy[EXEC_ARGV_MAX + 1];
  char argv_buf[EXEC_ARG_BYTES_MAX];
  const char *const *argv = NULL;
  int rc = sys_copy_user_string(path, sizeof(path), path_ptr);

  if (rc < 0) return (long)rc;
  rc = sys_execve_copy_user_argv(argv_copy, argv_buf, sizeof(argv_buf), argv_ptr);
  if (rc < 0) return (long)rc;
  if (argv_ptr != 0u) argv = argv_copy;

  /* Save old pages to free after successful load */
  page_id_t old_stack_id = current->stack_page_id;
  page_id_t old_user[USER_PAGES_MAX];
  proc_image_t old_image = current->image;
  int owns_pages = (current->vfork_parent == NULL);
  proc_copy_page_tracking_to_array(current, old_user);
#if defined(__m68k__)
  void *old_user_stack = current->user_stack_page;
#endif

  /* Clear pages so execve allocates fresh ones */
  current->stack_page_id = PAGE_ID_INVALID;
  proc_clear_page_tracking(current);
  current->image = (proc_image_t){0};
#if defined(__m68k__)
  current->user_stack_page = NULL;
#endif

  /* Save old cpu_state so we can free it after successful exec */
  /* Load the new binary.  argv points into the old stack/data pages
   * which are still valid (detached from current but not yet freed). */
  int err = exec_execve(current, path, argv);
  if (err < 0) {
    /* Restore old pages on failure — fds are untouched (POSIX) */
    current->stack_page_id = old_stack_id;
    proc_restore_page_tracking_from_array(current, old_user);
    current->image = old_image;
#if defined(__m68k__)
    current->user_stack_page = old_user_stack;
#endif
    return (long)err;
  }

  /* POSIX: preserve open fds across execve (redirections, pipes).
   * Only install default tty stdio if fd 0/1/2 are not already open
   * (e.g. init's first exec before any shell sets up fds). */
  if (current->fd_map[0] == FD_DESC_NONE &&
      current->fd_map[1] == FD_DESC_NONE &&
      current->fd_map[2] == FD_DESC_NONE)
    mod_vfs.fd_stdio_init(current);

    /* Free old kernel stack page.
     * m68k / RISC-V: kernel runs on stack_page.  sys_execve returns
     * through the old kernel stack before trap.S switches to the new
     * one via exec_pending.  Defer the free until after the switch.
     * ARM: kernel runs on MSP (separate), so PSP stack can be freed
     * immediately. */
#if defined(__m68k__) || defined(__riscv)
  extern volatile void *exec_old_stack;
  exec_old_stack = (old_stack_id != PAGE_ID_INVALID) ? mem_region_page_to_ptr(old_stack_id) : NULL;
#elif defined(__ia16__)
  if (old_stack_id != PAGE_ID_INVALID) mem_region_page_free(old_stack_id);
#else
  if (old_stack_id != PAGE_ID_INVALID) { void *os = mem_region_page_to_ptr(old_stack_id); proc_release_stack_page(&os); }
#endif

  /* Free old user pages only if we owned them */
  if (owns_pages) {
    image_release_owned_segments(&old_image);
    proc_release_tracked_pages_from_array(old_user);
#if defined(__m68k__)
    if (old_user_stack) proc_release_stack_page(&old_user_stack);
#endif
  } else if (current->vfork_parent) {
    /* vfork child: free pages that were allocated specifically for
     * the child (e.g. user stack copy), not the shared parent pages. */
    proc_release_private_tracked_pages_from_array(old_user,
                            current->vfork_parent->user_pages);
#if defined(__m68k__)
    if (old_user_stack &&
        old_user_stack != current->vfork_parent->user_stack_page)
      proc_release_stack_page(&old_user_stack);
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

/* ── sys_set_tid_address ─────────────────────────────────────────────────────
 */

long sys_set_tid_address(void *tidptr) {
  current->clear_child_tid = (int *)tidptr;
  return (long)current->pid;
}

/* ── sys_uname ────────────────────────────────────────────────────────────────
 */

/*
 * struct utsname layout (65 bytes per field × 6 fields = 390 bytes).
 * Matches Linux/musl: each field is char[65].
 */
#define UTS_LEN 65

long sys_uname(uintptr_t buf_ptr) {
  char out[UTS_LEN * 6];
  char *p;

  if (buf_ptr == 0u) return -(long)EINVAL;

  p = out;
  __builtin_memset(p, 0, sizeof(out));

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

  if (sys_copy_to_user(buf_ptr, out, sizeof(out)) < 0) return -(long)EFAULT;
  return 0;
}

/* ── sys_setpgid ──────────────────────────────────────────────────────────────
 */

long sys_setpgid(long pid, long pgid) {
  pcb_t *target;

  if (pid == 0)
    target = current;
  else {
    target = NULL;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
      if (proc_table[i].state != PROC_FREE && proc_table[i].pid == (pid_t)pid) {
        target = &proc_table[i];
        break;
      }
    }
    if (!target) return -(long)ESRCH;
  }

  target->pgid = (pgid == 0) ? target->pid : (pid_t)pgid;
  return 0;
}

/* ── sys_setsid ───────────────────────────────────────────────────────────────
 */

long sys_setsid(void) {
  current->sid = current->pid;
  current->pgid = current->pid;
  return (long)current->pid;
}

/* ── sys_wait4 ────────────────────────────────────────────────────────────────
 */

long sys_wait4(long pid, long status_ptr, long options, uintptr_t rusage_ptr) {
  (void)rusage_ptr; /* rusage not supported */
  return sys_waitpid(pid, status_ptr, options);
}
