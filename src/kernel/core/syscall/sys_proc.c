/*
 * sys_proc.c — Process-related syscall implementations
 *
 *   sys_exit(status)    — terminate the calling process
 *   sys_getpid()        — return the calling process's PID
 *   sys_vfork(frame)    — create child process (parent blocked)
 *   sys_waitpid(pid,st) — wait for child to exit, reap zombie
 *   sys_execve(path,argv) — replace process image with new ELF binary
 */

#include <stddef.h>
#include <string.h>

#include "common/errno.h"
#include "common/ptrace.h"
#include "common/utsname.h"
#include "common/wait.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/common/spinlock.h"
#include "kernel/common/version.h"
#include "kernel/core/arch.h"
#include "kernel/core/cpu/cpu.h"
#include "kernel/core/cpu/ecpu_m68k.h"
#include "kernel/core/cpu/ecpu_z80.h"
#include "kernel/core/exec/exec.h"
#include "kernel/core/exec/exec_args.h"
#include "kernel/core/mm/page.h"
#include "kernel/core/mm/page_io.h"
#include "kernel/core/mm/page_ptr.h"
#include "kernel/core/mm/region.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/proc/sched.h"
#include "kernel/core/signal/signal.h"
#include "kernel/core/subsys/ppap/ppap_m68k_bridge.h"
#include "kernel/core/subsys/subsys.h"
#include "kernel/core/syscall/syscall.h"
#include "target/target.h"

/* Wait status encoding (POSIX-compatible) */
#define W_EXITCODE(ret) (((ret) & 0xff) << 8)
#define W_STOPCODE(sig) ((((sig) & 0xff) << 8) | 0x7f)

#define TRACE_PHASE_ENTER 0
#define TRACE_PHASE_EXIT 1

#ifndef ARCH_EXIT_SWITCH_IN_SYSCALL_EPILOGUE
#define ARCH_EXIT_SWITCH_IN_SYSCALL_EPILOGUE 0
#endif
#define TRACE_MODE_MASK \
  (PPAP_TRACE_MODE_PPAP_SYSCALL | PPAP_TRACE_MODE_SUBSYS_CALL)

#define EXEC_SNAPSHOT_IMAGE_OFF 0u /* saved proc_image_t */
#define EXEC_SNAPSHOT_USER_OFF \
  ((uint16_t)sizeof(proc_image_t)) /* saved user_pages[] */
#define EXEC_SNAPSHOT_TOTAL_BYTES \
  (sizeof(proc_image_t) + USER_PAGES_MAX * sizeof(page_id_t))

_Static_assert(EXEC_SNAPSHOT_TOTAL_BYTES <= PAGE_SIZE,
               "execve snapshot must fit in one page");

/* Release an image segment if it is OWNED (independently allocated).
 * Non-OWNED segments (XIP, sub-pointers into another allocation) are
 * just cleared.  In shared-stack exec models, image.data may own the
 * full user segment while user_pages[] only tracks logical occupancy. */
static void image_segment_release_owned(proc_image_segment_t *seg) {
  if (!seg || !seg->base) return;
  if (seg->flags & PROC_IMAGE_SEG_OWNED) {
    region_t r = {seg->base, seg->base_page};
    region_free(seg->mem_class, seg->size, &r);
  }
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

static int image_segment_contains_page_id(const proc_image_segment_t *seg,
                                          page_id_t page_id) {
  uint32_t n_pages;

  if (!seg || seg->size == 0) return 0;
  if (!(seg->flags & PROC_IMAGE_SEG_OWNED)) return 0;
  if (seg->base_page == PAGE_ID_INVALID) return 0;

  n_pages = (seg->size + PAGE_SIZE - 1u) / PAGE_SIZE;
  return page_id >= seg->base_page && page_id < seg->base_page + n_pages;
}

static int image_contains_owned_page_id(const proc_image_t *image,
                                        page_id_t page_id) {
  if (!image || page_id == PAGE_ID_INVALID) return 0;

  return image_segment_contains_page_id(&image->text, page_id) ||
         image_segment_contains_page_id(&image->staged_text, page_id) ||
         image_segment_contains_page_id(&image->staged_rodata, page_id) ||
         image_segment_contains_page_id(&image->literal, page_id) ||
         image_segment_contains_page_id(&image->rodata, page_id) ||
         image_segment_contains_page_id(&image->data, page_id);
}

static int exec_snapshot_segment_contains_page_id(page_id_t snapshot_page,
                                                  uint16_t seg_off,
                                                  page_id_t page_id) {
  proc_image_segment_t seg;

  if (snapshot_page == PAGE_ID_INVALID) return 0;
  if (page_id == PAGE_ID_INVALID) return 0;
  page_read(snapshot_page, seg_off, &seg, sizeof(seg));
  return image_segment_contains_page_id(&seg, page_id);
}

static int exec_snapshot_contains_owned_page_id(page_id_t snapshot_page,
                                                page_id_t page_id) {
  if (snapshot_page == PAGE_ID_INVALID) return 0;
  if (page_id == PAGE_ID_INVALID) return 0;

  return exec_snapshot_segment_contains_page_id(
             snapshot_page, (uint16_t)offsetof(proc_image_t, text), page_id) ||
         exec_snapshot_segment_contains_page_id(
             snapshot_page, (uint16_t)offsetof(proc_image_t, staged_text),
             page_id) ||
         exec_snapshot_segment_contains_page_id(
             snapshot_page, (uint16_t)offsetof(proc_image_t, staged_rodata),
             page_id) ||
         exec_snapshot_segment_contains_page_id(
             snapshot_page, (uint16_t)offsetof(proc_image_t, literal),
             page_id) ||
         exec_snapshot_segment_contains_page_id(
             snapshot_page, (uint16_t)offsetof(proc_image_t, rodata),
             page_id) ||
         exec_snapshot_segment_contains_page_id(
             snapshot_page, (uint16_t)offsetof(proc_image_t, data), page_id);
}

static void proc_untrack_owned_segment_pages(pcb_t *p) {
  if (!p) return;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) {
    if (p->user_pages[i] == PAGE_ID_INVALID) continue;
    if (image_contains_owned_page_id(&p->image, p->user_pages[i]))
      p->user_pages[i] = PAGE_ID_INVALID;
  }
}

static int exec_snapshot_save(page_id_t *snapshot_page, const pcb_t *p) {
  page_id_t page;

  if (!snapshot_page || !p) return -(int)EINVAL;
  page = page_alloc();
  if (page == PAGE_ID_INVALID) return -(int)ENOMEM;

  page_write(page, EXEC_SNAPSHOT_IMAGE_OFF, &p->image, sizeof(p->image));
  page_write(page, EXEC_SNAPSHOT_USER_OFF, p->user_pages,
             sizeof(p->user_pages));
  *snapshot_page = page;
  return 0;
}

static void exec_snapshot_restore(page_id_t snapshot_page, pcb_t *p) {
  if (snapshot_page == PAGE_ID_INVALID || !p) return;
  page_read(snapshot_page, EXEC_SNAPSHOT_IMAGE_OFF, &p->image,
            sizeof(p->image));
  page_read(snapshot_page, EXEC_SNAPSHOT_USER_OFF, p->user_pages,
            sizeof(p->user_pages));
}

static void exec_snapshot_release_owned_segments(page_id_t snapshot_page) {
  proc_image_t image;

  if (snapshot_page == PAGE_ID_INVALID) return;
  page_read(snapshot_page, EXEC_SNAPSHOT_IMAGE_OFF, &image, sizeof(image));
  image_release_owned_segments(&image);
}

static void exec_snapshot_release_tracked_pages(page_id_t snapshot_page) {
  if (snapshot_page == PAGE_ID_INVALID) return;

  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) {
    page_id_t page_id;

    page_read(snapshot_page,
              (uint16_t)(EXEC_SNAPSHOT_USER_OFF + i * sizeof(page_id_t)),
              &page_id, sizeof(page_id));
    if (page_id == PAGE_ID_INVALID) continue;
    if (exec_snapshot_contains_owned_page_id(snapshot_page, page_id)) continue;
    page_free(page_id);
  }
}

static void exec_snapshot_release_private_tracked_pages(
    page_id_t snapshot_page, const page_id_t shared[USER_PAGES_MAX]) {
  if (snapshot_page == PAGE_ID_INVALID) return;

  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) {
    page_id_t page_id;

    page_read(snapshot_page,
              (uint16_t)(EXEC_SNAPSHOT_USER_OFF + i * sizeof(page_id_t)),
              &page_id, sizeof(page_id));
    if (page_id == PAGE_ID_INVALID) continue;
    if (shared && shared[i] == page_id) continue;
    if (exec_snapshot_contains_owned_page_id(snapshot_page, page_id)) continue;
    page_free(page_id);
  }
}

static void proc_release_stack_page(void **page) {
  if (!page || !*page) return;
  region_t r = {*page, page_from_ptr(*page)};
  region_free(PPAP_MEM_RAM_STACK, PAGE_SIZE, &r);
  *page = NULL;
}

static void proc_wake_blocked_locked(pcb_t *p) {
  if (!p || p->state != PROC_BLOCKED) return;
  p->state = PROC_RUNNABLE;
  p->wait_channel = NULL;
}

static void proc_wake_blocked_pid_locked(pid_t pid) {
  for (uint32_t i = 0; i < PROC_MAX; i++) {
    pcb_t *p = &proc_table[i];
    if (p->state == PROC_FREE || p->pid != pid) continue;
    proc_wake_blocked_locked(p);
    break;
  }
}

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

static void trace_stop_current(void) {
  current->trace_wait_pending = 1;
  current->state = PROC_TRACED_STOP;
  trace_wake_tracer(current);
  /* Block until the tracer flips us back to PROC_RUNNABLE (via
   * trace_resume_target or sys_kill).  This is continuation blocking —
   * no syscall_restart / PC-rewind dance — so the caller resumes inline
   * and any in-flight syscall body proceeds after we return. */
  while (current->state != PROC_RUNNABLE) sched_switch();
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
  sched_switch();
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
  if (target->user_stack_page) {
    uint32_t base = (uint32_t)(uintptr_t)target->user_stack_page;
    if (addr >= base && addr < base + PAGE_SIZE) return 1;
  }
#if !defined(__ia16__)
  if (target->stack_page_id != PAGE_ID_INVALID) {
    uint32_t base = (uint32_t)(uintptr_t)page_to_ptr(target->stack_page_id);
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

void trace_before_syscall(uint32_t *frame, uint32_t nr, uint32_t a4,
                          uint32_t a5) {
  if (!(current->trace_mode & PPAP_TRACE_MODE_PPAP_SYSCALL)) return;
  if (current->trace_syscall_phase != TRACE_PHASE_ENTER) return;
  if (nr == SYS_PTRACE) return;

  current->trace_syscall_phase = TRACE_PHASE_EXIT;
  trace_fill_event(PPAP_TRACE_EVENT_SYSCALL_ENTER, PPAP_TRACE_ABI_PPAP, nr,
                   frame[0], frame[1], frame[2], frame[3], a4, a5, 0, 0);
  trace_stop_current();
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
  trace_stop_current();
}

void trace_before_subsys(uint32_t abi, uint32_t nr, uint32_t a0, uint32_t a1,
                         uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
  if (!(current->trace_mode & PPAP_TRACE_MODE_SUBSYS_CALL)) return;
  if (current->trace_subsys_phase != TRACE_PHASE_ENTER) return;

  current->trace_subsys_phase = TRACE_PHASE_EXIT;
  trace_fill_event(PPAP_TRACE_EVENT_SUBSYS_ENTER, abi, nr, a0, a1, a2, a3, a4,
                   a5, 0, 0);
  trace_stop_current();
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
  trace_stop_current();
}

void trace_debug_stop(uint32_t abi, uint32_t pc, uint32_t flags) {
  trace_fill_event(PPAP_TRACE_EVENT_DEBUG_STOP, abi, 0, pc, 0, 0, 0, 0, 0, 0,
                   flags);
  trace_stop_current();
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
  trace_stop_current();
}

#if defined(__ia16__)
/* Stub — ptrace not supported on i16 (saves ~4 KB text) */
long sys_ptrace(long req, long pid, uintptr_t addr, uintptr_t data_ptr) {
  (void)req;
  (void)pid;
  (void)addr;
  (void)data_ptr;
  return -(long)ENOSYS;
}
#else
static long ptrace_copy_in(void *dst, uintptr_t user_ptr, size_t len) {
  if (user_ptr == 0u) return -(long)EINVAL;
  if (sys_copy_from_user(dst, user_ptr, len) < 0) return -(long)EFAULT;
  return 0;
}

static long ptrace_copy_out(uintptr_t user_ptr, const void *src, size_t len) {
  if (user_ptr == 0u) return -(long)EINVAL;
  if (sys_copy_to_user(user_ptr, src, len) < 0) return -(long)EFAULT;
  return 0;
}

long sys_ptrace(long req, long pid, uintptr_t addr, uintptr_t data_ptr) {
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
    case PTRACE_GETEVENT: {
      struct ppap_ptrace_event event = target->trace_event;

      return ptrace_copy_out(data_ptr, &event, sizeof(event));
    }
    case PTRACE_PEEKDATA: {
      uint32_t word;
      int rc;

      if (data_ptr == 0u) return -(long)EINVAL;
      rc = trace_read32(target, (uint32_t)addr, &word);
      if (rc < 0) return rc;
      return ptrace_copy_out(data_ptr, &word, sizeof(word));
    }
    case PTRACE_POKEDATA: {
      uint32_t word;
      long rc = ptrace_copy_in(&word, data_ptr, sizeof(word));

      if (rc < 0) return rc;
      return trace_write32(target, (uint32_t)addr, word);
    }
    case PTRACE_GETREGS: {
      struct ppap_ptrace_regs regs;
      int rc = trace_fill_regs(target, &regs);

      if (rc < 0) return rc;
      return ptrace_copy_out(data_ptr, &regs, sizeof(regs));
    }
    case PTRACE_SETREGS: {
      struct ppap_ptrace_regs regs;
      long rc = ptrace_copy_in(&regs, data_ptr, sizeof(regs));

      if (rc < 0) return rc;
      rc = trace_store_regs(target, &regs);
      if (rc == 0) target->trace_swbp_skip_once = 0;
      return rc;
    }
    case PTRACE_GETCAPS: {
      struct ppap_ptrace_caps caps;
      int rc = trace_fill_caps(target, &caps);

      if (rc < 0) return rc;
      return ptrace_copy_out(data_ptr, &caps, sizeof(caps));
    }
    case PTRACE_GETSURFACE: {
      uint32_t surface = trace_active_surface_for(target);

      return ptrace_copy_out(data_ptr, &surface, sizeof(surface));
    }
    case PTRACE_SETSURFACE:
      return trace_set_surface(target, (uint32_t)addr);
    case PTRACE_SETMODE: {
      uint8_t mode = (uint8_t)addr;
      if (mode & (uint8_t)~TRACE_MODE_MASK) return -(long)EINVAL;
      trace_reset_mode_state(target, mode);
      return 0;
    }
    case PTRACE_SETBP: {
      struct ppap_ptrace_bp bp;
      int rc;

      if (data_ptr == 0u) return -(long)EINVAL;
      if (sys_copy_from_user(&bp, data_ptr, sizeof(bp)) < 0)
        return -(long)EFAULT;
      rc = trace_set_bp(target, &bp);
      if (rc < 0) return rc;
      if (sys_copy_to_user(data_ptr, &bp, sizeof(bp)) < 0) return -(long)EFAULT;
      return rc;
    }
    case PTRACE_CLRBP: {
      struct ppap_ptrace_bp bp;
      int rc;

      if (data_ptr == 0u) return -(long)EINVAL;
      if (sys_copy_from_user(&bp, data_ptr, sizeof(bp)) < 0)
        return -(long)EFAULT;
      rc = trace_clr_bp(target, &bp);
      if (rc < 0) return rc;
      if (sys_copy_to_user(data_ptr, &bp, sizeof(bp)) < 0) return -(long)EFAULT;
      return rc;
    }
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

/* ── sys_poweroff ─────────────────────────────────────────────────────────────
 */

long sys_poweroff(void) {
  target_may_poweroff(0);
  return 0;
}

void kernel_panic_halt(uint8_t status) { target_may_poweroff(status); }

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
 * Note: after the process is marked ZOMBIE, either sys_exit switches away
 * directly or the architecture's syscall epilogue performs the switch.  There
 * is no need for an infinite loop — the ZOMBIE process will never be
 * scheduled again because sched_next() only picks PROC_RUNNABLE processes.
 */
long sys_exit(long status) {
  /* Guard against double-exit.  Some user-space _Exit() paths issue
   * SYS_exit_group then SYS_exit in a loop.  On RISC-V, the second
   * call can reach here if the context switch after the first exit
   * doesn't happen before the ecall return path re-executes. */
  if (current->state == PROC_ZOMBIE) {
#if ARCH_EXIT_SWITCH_IN_SYSCALL_EPILOGUE
    /* The syscall epilogue performs the post-syscall switch when it sees
     * !PROC_RUNNABLE. Triggering a nested cooperative yield here can bounce
     * back to user stub loops on such architectures. */
    return 0;
#else
    sched_switch();
    return 0; /* unreachable — zombie won't be scheduled */
#endif
  }

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
    proc_untrack_owned_segment_pages(current);
#if !defined(__ia16__)
    if (current->stack_page_id != PAGE_ID_INVALID &&
        proc_page_backed_contains(
            current, (uintptr_t)page_to_ptr(current->stack_page_id)))
      current->stack_page_id = PAGE_ID_INVALID;
#endif
    proc_release_tracked_pages(current, 0, USER_PAGES_MAX);
    if (current->user_stack_page) {
      proc_release_stack_page(&current->user_stack_page);
    }
  } else {
    /* vfork child exiting without exec: free child-owned pages only.
     * In the no-copy vfork model the child shares the parent's
     * user_stack_page and stack_page_id; the parent owns them. */
    proc_release_private_tracked_pages(current, current->vfork_parent);
    if (current->stack_page_id == current->vfork_parent->stack_page_id)
      current->stack_page_id = PAGE_ID_INVALID;
  }

#ifdef KSTACK_USAGE_TRACK
  proc_kstack_usage_report();
#endif

  uint32_t saved = spin_lock_irqsave(SPIN_PROC);
  current->exit_status = (int)status;

  /* Unblock vfork parent if we are a vfork child. */
  if (current->vfork_parent) {
    proc_wake_blocked_locked(current->vfork_parent);
    current->vfork_parent = NULL;
  }

  /* Wake parent if it is blocked in waitpid.  After execve, vfork_parent is
   * NULL, so this wake is still needed for ordinary waitpid completion. */
  proc_wake_blocked_pid_locked(current->ppid);
  if (current->tracer_pid != 0 && current->tracer_pid != current->ppid)
    proc_wake_blocked_pid_locked(current->tracer_pid);

  /* Reparent children to init (PID 1) so they can be reaped.  If a reparented
   * child is already zombie, wake init. */
  for (uint32_t i = 1; i < PROC_MAX; i++) {
    pcb_t *child = &proc_table[i];
    if (child->state == PROC_FREE || child->ppid != current->pid) continue;
    child->ppid = 1;
    if (child->state == PROC_ZOMBIE) proc_wake_blocked_pid_locked(1);
  }

  current->state = PROC_ZOMBIE;
  spin_unlock_irqrestore(SPIN_PROC, saved);
#if ARCH_EXIT_SWITCH_IN_SYSCALL_EPILOGUE
  /* Return to the syscall epilogue and let it switch away based on
   * current->state != PROC_RUNNABLE. */
  return 0;
#else
  sched_switch();
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
 * The child gets its own saved context with a copy of the parent's exception
 * or trap frame (return register = 0 for child). The child shares the
 * parent's tracked page-backed slots (GOT/data).
 *
 * frame: pointer to the parent's stacked exception frame
 *   ARM:  [r0, r1, r2, r3, r12, lr, pc, xpsr] on PSP
 *   m68k: &regs[1] (d1 slot) in the TRAP #0 saved register frame
 */
long sys_vfork(uint32_t *frame) {
  /* 1. Allocate child PCB */
  pcb_t *child = proc_alloc();
  if (!child) return -(long)ENOMEM;

  /* 2. Share parent's user_pages with child */
  proc_copy_page_tracking(child, current);

  /* 3. Build child's saved context.  Every supported arch runs the no-copy
   * vfork model: the child shares the parent's user_stack_page and the
   * per-arch code below builds a child kernel-stack frame whose user-mode
   * resume point is the instruction after the vfork trap.  See
   * docs/proposals/no_stack_copy_on_vfork.md.
   */
#if defined(__ia16__)
  /* i16 split-frame layout: GP+IRET (24B) on user stack, user_SS:SP
   * (4B) on kernel stack.  Child shares parent's user stack (vfork).
   *
   * Save parent's 24B user frame to parent's kernel stack so it
   * survives the child's execve overwriting the user stack.
   * Build the child's kernel stack with the same user_SS:SP. */
  {
    /* Read parent's user_SS:SP from kernel stack.
     * i16_trap_frame_sp was captured by trap.S at syscall entry. */
    uint16_t *trap_ksp = (uint16_t *)(uintptr_t)i16_trap_frame_sp;
    uint16_t parent_user_sp = trap_ksp[0];
    uint16_t parent_user_ss = trap_ksp[1];

    /* Compute the user frame's page location */
    uint32_t frame_linear = (uint32_t)parent_user_ss * 16 + parent_user_sp;
    uint32_t data_base = page_linear(current->image.data.base_page);
    uint32_t rel = frame_linear - data_base;
    page_id_t frame_page =
        current->image.data.base_page + (page_id_t)(rel / PAGE_SIZE);
    uint16_t frame_page_off = (uint16_t)(rel % PAGE_SIZE);

    /* Save parent's 34B user frame onto the parent's OWN kernel
     * stack, *below* trap_ksp.  The 34 bytes cover:
     *   - 24B GP+IRET frame (ES..FLAGS, popped by trap.S restore)
     *   - 10B vfork stub frame (saved DI, SI, BX, BP + return addr,
     *     popped by SYSCALL_RET after iret)
     *
     * The child's execve call inevitably pushes args and a return
     * address onto the shared user stack, overwriting those 10 bytes
     * above the GP+IRET frame.  Saving 34B covers the full region
     * that the child cannot avoid clobbering.
     *
     * Patch the AX slot (offset 16) with child PID so the parent
     * sees the correct vfork return value when the frame is restored. */
    uint8_t saved_frame[34];
    page_read(frame_page, frame_page_off, saved_frame, 34);
    uint16_t child_pid16 = (uint16_t)child->pid;
    saved_frame[16] = (uint8_t)(child_pid16 & 0xFF);
    saved_frame[17] = (uint8_t)(child_pid16 >> 8);
    uint8_t *kstack_save =
        (uint8_t *)(uintptr_t)((uint16_t)(uintptr_t)trap_ksp - 34u);
    __builtin_memcpy(kstack_save, saved_frame, 34);
    current->vfork_frame_saved = 1;

    /* Build child's kernel stack: [user_SP, user_SS] at the top, then
     * the 34-byte vfork-save slot reserved by the trap.S/switch.S
     * convention.  child->sp points at the post-reserve position. */
    uint16_t child_ksp = child->kernel_sp;
    uint16_t *ckf = (uint16_t *)(uintptr_t)(child_ksp - 4);
    ckf[0] = parent_user_sp;
    ckf[1] = parent_user_ss;
    child->sp = (uint32_t)(uint16_t)(child_ksp - 4u - 34u);

    /* Patch AX=0 in shared user stack frame for child return */
    uint16_t zero = 0;
    uint16_t ax_rel = (uint16_t)(rel + 16);
    page_id_t ax_page =
        current->image.data.base_page + (page_id_t)(ax_rel / PAGE_SIZE);
    uint16_t ax_off = ax_rel % PAGE_SIZE;
    page_write(ax_page, ax_off, &zero, 2);
  }
#elif defined(__m68k__)
  /* m68k: The TRAP #0 frame (15 regs + SR + PC) has the same layout as
   * the switch.S context frame.  The child returns directly to user code
   * via switch.S restore (movem.l + rte), bypassing TRAP #0 cleanup.
   *
   * frame = &regs[1] (d1 slot).  Copy the live frame range from the
   * parent's fixed kstack to the child's fixed kstack at the same top
   * distance, then patch the child's d0/a5 slots.
   *
   * No-copy vfork: the child shares the parent's user_stack_page.  m68k
   * trap #0 pushes nothing on USP, and the vfork stub pushes nothing
   * either, but the parent's bsr-pushed return address (4 bytes at
   * parent->usp+0..3) is exactly the address the child's first execve-arg
   * push writes to.  m68k_vfork_save_parent_frame() saves those 4 bytes
   * so m68k_vfork_restore_frame() can put them back before the parent's
   * next user-mode rte.  a6 (frame pointer) stays valid because parent
   * and child point at the same user_stack_page — no remap. */
  uint32_t *parent_regs = frame - 1; /* d0 slot */
  uintptr_t parent_top = (uintptr_t)current->kernel_sp;
  uintptr_t parent_base = (uintptr_t)parent_regs;
  uint32_t frame_bytes = (uint32_t)(parent_top - parent_base);
  uint32_t *child_regs =
      (uint32_t *)(uintptr_t)(child->kernel_sp - frame_bytes);
  memcpy(child_regs, parent_regs, frame_bytes);
  child_regs[0] = 0;                  /* d0 = 0 (child return) */
  child_regs[13] = current->got_base; /* a5 = GOT base for PIC */

  child->sp = (uint32_t)(uintptr_t)child_regs;
  child->user_stack_page = current->user_stack_page;
  child->usp = current->usp;

  m68k_vfork_save_parent_frame(current);
#elif defined(__riscv)
  /* RISC-V: The ecall trap frame (36 words) lives on the parent's fixed
   * kernel stack.  Copy that frame into the child's fixed kstack slot and
   * patch the child's saved a0 to 0.
   *
   * frame points to saved a0 in the parent's trap frame (trap_base + 32).
   *
   * No-copy vfork: the child shares the parent's user_stack_page.  The
   * RISC-V ecall ABI pushes nothing on the user stack, and the vfork /
   * execve / _exit stubs push nothing either, so the parent's resume
   * state — which lives in the parent's own trap frame on the parent's
   * kstack slot — is not reachable from the child's path.  TF_USER_SP in
   * the child's trap frame already points at the parent's user sp from
   * the memcpy above; no remap is needed. */
  uint32_t *parent_tf = frame - 8; /* trap frame base */
  uint32_t *child_tf =
      (uint32_t *)(uintptr_t)(child->kernel_sp - 36u * sizeof(uint32_t));
  memcpy(child_tf, parent_tf, 36u * sizeof(uint32_t));
  child_tf[8] = 0; /* saved a0 = 0 — child sees vfork() return 0 */

  child->sp = (uint32_t)(uintptr_t)child_tf;
  child->user_stack_page = current->user_stack_page;

#elif defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
  /* No-copy vfork: the child shares the parent's user_stack_page.  Build
   * the child's 10-word SW frame on the child's kstack slot, pointing
   * the saved-PSP slot at the parent's HW frame on the shared user stack.
   * The save/patch happens AFTER Step 7 so the saved slot 0 captures
   * r0 = child_pid — see arm_vfork_save_parent_frame below. */
  uint32_t *sw = (uint32_t *)(uintptr_t)child->kernel_sp;
  sw = arch_build_initial_frame(sw, NULL);
  /* sw layout: 0..7=r4..r11, 8=EXC_RETURN, 9=saved PSP. */
  sw[5] = current->got_base;          /* r9 = GOT SRAM addr for PIC */
  sw[9] = (uint32_t)(uintptr_t)frame; /* saved PSP = parent's HW frame */
  child->sp = (uint32_t)(uintptr_t)sw;
  child->user_stack_page = current->user_stack_page;

#elif defined(__xtensa__)
  /* Xtensa: Build a new-process frame for the child.
   *
   * XtExcFrame layout: exit, pc, ps, a0, a1, a2, ...
   * 'frame' points to a2, so frame[-4] = pc, frame[-1] = a1.
   *
   * The child resumes at the instruction after the ILL trap (pc already +3)
   * with a2 = 0 (switch.S .Lnew_process clears a2 before jx).
   *
   * No-copy vfork: the child shares the parent's user_stack_page.  ESP-IDF
   * exception entry leaves a windowed continuation below XtExcFrame on that
   * stack, and the child's next exception can overwrite either.  Step 7
   * therefore saves the vulnerable resume slice after patching the parent's
   * a2 return slot.  The syscall epilogue restores it before the parent
   * returns to user mode. */
  {
    uint32_t child_pc = frame[-4];      /* pc (already advanced +3) */
    uint32_t child_user_sp = frame[-1]; /* a1 = user SP at syscall */
    uint32_t child_a0 = frame[-2];      /* a0 = return addr to caller */
    uint32_t child_a3 = frame[1];       /* a3 (compiler expects preserved) */

    void *parent_ustack = proc_user_stack_base(current);
    if (!parent_ustack) {
      proc_free(child);
      return -(long)ENOMEM;
    }

    child->user_stack_page = parent_ustack;

    uint32_t *sp = xtensa_build_vfork_child_frame(
        child->kernel_sp, child_pc, child_user_sp, child_a0, child_a3);
    child->sp = (uint32_t)(uintptr_t)sp;
  }

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

#if defined(__xtensa__)
  xtensa_vfork_save_parent_frame(current, frame - 5, xtensa_trap_entry_low_sp);
#endif

#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
  /* 7a. No-copy vfork on arm: the HW frame lives in shared user memory.
   * Save the 40 B vulnerable region (now containing r0 = child_pid from
   * Step 7) into the parent's PCB and overwrite live user_PSP+0 with 0
   * so the child's HW frame pop yields r0 = 0.
   * arm_vfork_restore_frame() writes the saved 40 B back before the
   * parent's next user-mode bx EXC_RETURN. */
  arm_vfork_save_parent_frame(current, frame);
#endif

  /* 8. Block parent, make child runnable */
  sched_block_current(NULL);
  child->state = PROC_RUNNABLE;

  /* 9. Yield to the child.  Some architectures switch immediately here;
   * others switch from the syscall/trap return path.  The parent is BLOCKED,
   * so it will not be selected again until the child exits or execs. */
  sched_switch();

  /* Clear stale exec_pending left by the child's execve.  exec_pending is
   * global (not per-process), so a child's execve can leave a flag that
   * would corrupt our return. */
  exec_pending[core_id()] = 0;

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
 * Returns child PID on success, -ECHILD if no children, 0 if WNOHANG,
 * -EINTR if interrupted by a signal.
 */
long sys_waitpid(long pid, long status_ptr, long options) {
  int deferred_timer_armed = 0;

  for (;;) {
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
        if (sys_copy_to_user((uintptr_t)status_ptr, &status, sizeof(status)) <
            0)
          return -(long)EFAULT;
      }
      stopped->trace_wait_pending = 0;
      return (long)stopped->pid;
    }

    if (zombie) {
      pid_t cpid = zombie->pid;

      if (status_ptr) {
        int status = W_EXITCODE(zombie->exit_status);
        if (sys_copy_to_user((uintptr_t)status_ptr, &status, sizeof(status)) <
            0)
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
        page_free(zombie->stack_page_id);
#else
        {
          void *zs = page_to_ptr(zombie->stack_page_id);
          proc_release_stack_page(&zs);
        }
#endif
        zombie->stack_page_id = PAGE_ID_INVALID;
      }

      proc_free(zombie);
      return (long)cpid;
    }

    if (!has_match) return -(long)ECHILD;

    if (options & WNOHANG) return 0;

    if (current->sig_pending & ~current->sig_blocked) return -(long)EINTR;

    if (!deferred_timer_armed) {
      target_enable_deferred_timer();
      deferred_timer_armed = 1;
    }

    /* Block until sys_exit / sys_kill / trace_stop sets PROC_RUNNABLE. */
    sched_sleep_current(NULL);
  }
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
/* Walk a user-space NULL-terminated string vector (argv or envp), copying
 * each string straight into the args page via sys_copy_user_string_to_page
 * and committing it through the streaming exec_args_*_begin/commit pair.
 * Returns 0 on success, -errno on user-fault, vector overflow, or
 * insufficient args-page capacity. */
static int sys_execve_copy_user_vec(exec_args_t *args, uintptr_t user_ptr,
                                    int is_envp) {
  if (user_ptr == 0u) return 0;
  for (;;) {
    uintptr_t str_ptr;
    int rc = sys_copy_from_user(&str_ptr, user_ptr, sizeof(str_ptr));
    if (rc < 0) return rc;
    if (str_ptr == 0u) return 0;
    page_id_t dst_page;
    uint16_t dst_off;
    uint16_t avail;
    rc = is_envp ? exec_args_envp_begin(args, &dst_page, &dst_off, &avail)
                 : exec_args_argv_begin(args, &dst_page, &dst_off, &avail);
    if (rc < 0) return rc;
    int n = sys_copy_user_string_to_page(dst_page, dst_off, avail, str_ptr);
    if (n < 0) return (n == -(long)ENAMETOOLONG) ? -(long)E2BIG : n;
    rc = is_envp ? exec_args_envp_commit(args, (uint16_t)n)
                 : exec_args_argv_commit(args, (uint16_t)n);
    if (rc < 0) return rc;
    user_ptr += sizeof(str_ptr);
  }
}

long sys_execve(page_id_t path_page, uint16_t path_off, uintptr_t argv_ptr,
                uintptr_t envp_ptr) {
  exec_args_t args;
  page_id_t exec_snapshot = PAGE_ID_INVALID;

  /* Allocate a single page from the data region for the captured
   * (path, argv, envp) payload.  All access goes through
   * page_read/write, so the same code runs on 32-bit and ia16. */
  page_id_t args_page = page_alloc();
  if (args_page == PAGE_ID_INVALID) return -(long)ENOMEM;
  exec_args_init(&args, args_page);

  /* Path: copy byte-by-byte from the user (page, off) reference into the
   * args page's path slot. */
  {
    user_page_ref_t pref = {.page = path_page, .off = path_off};
    uint16_t i;
    for (i = 0; i < VFS_PATH_MAX; i++) {
      uint8_t c;
      page_read(pref.page, pref.off, &c, 1);
      page_write(args_page, (uint16_t)(EXEC_ARGS_PATH_OFF + i), &c, 1);
      if (c == 0) break;
      if (++pref.off >= PAGE_SIZE) {
        pref.page++;
        pref.off = 0;
      }
    }
    if (i >= VFS_PATH_MAX) {
      page_free(args_page);
      return -(long)ENAMETOOLONG;
    }
  }

  int rc = sys_execve_copy_user_vec(&args, argv_ptr, /*is_envp=*/0);
  if (rc < 0) {
    page_free(args_page);
    return (long)rc;
  }
  rc = sys_execve_copy_user_vec(&args, envp_ptr, /*is_envp=*/1);
  if (rc < 0) {
    page_free(args_page);
    return (long)rc;
  }

  /* Default-argv: if the caller passed argv == NULL or an empty vector,
   * synthesize argv[0] = path via an intra-args-page copy (path slot →
   * argv slot).  No kernel staging buffer needed. */
  if (args.argc == 0) {
    page_id_t dst_page;
    uint16_t dst_off;
    uint16_t dst_max;
    rc = exec_args_argv_begin(&args, &dst_page, &dst_off, &dst_max);
    if (rc < 0) {
      page_free(args_page);
      return (long)rc;
    }
    int plen = exec_args_path_to_page(&args, dst_page, dst_off, dst_max);
    if (plen < 0) {
      page_free(args_page);
      return (long)plen;
    }
    rc = exec_args_argv_commit(&args, (uint16_t)plen);
    if (rc < 0) {
      page_free(args_page);
      return (long)rc;
    }
  }

  /* Save old pages to free after successful load */
  page_id_t old_stack_id = current->stack_page_id;
  int owns_pages = (current->vfork_parent == NULL);
  rc = exec_snapshot_save(&exec_snapshot, current);
  if (rc < 0) {
    page_free(args_page);
    return (long)rc;
  }
  void *old_user_stack = current->user_stack_page;

  /* Clear pages so execve allocates fresh ones */
  current->stack_page_id = PAGE_ID_INVALID;
  proc_clear_page_tracking(current);
  current->image = (proc_image_t){0};
  current->user_stack_page = NULL;

  /* Save old cpu_state so we can free it after successful exec.  argv/envp
   * live in the args page, which we keep allocated until exec_execve
   * returns (loaders consume it inline). */
  int err = exec_execve(current, &args);
  if (err < 0) {
    /* Restore old pages on failure — fds are untouched (POSIX) */
    current->stack_page_id = old_stack_id;
    exec_snapshot_restore(exec_snapshot, current);
    current->user_stack_page = old_user_stack;
    page_free(exec_snapshot);
    page_free(args_page);
    return (long)err;
  }
  /* exec_execve has consumed args; release the page before we wire up
   * the rest of the new image. */
  page_free(args_page);

  /* POSIX: preserve open fds across execve (redirections, pipes).
   * Only install default tty stdio if fd 0/1/2 are not already open
   * (e.g. init's first exec before any shell sets up fds). */
  if (current->fd_map[0] == FD_DESC_NONE &&
      current->fd_map[1] == FD_DESC_NONE &&
      current->fd_map[2] == FD_DESC_NONE) {
    mod_vfs.fd_stdio_init(current);
  }

  /* Free old page-backed stack, if the previous image allocated one.
   * m68k and RISC-V native ELF processes use fixed kernel stacks now, so
   * stack_page_id is either invalid or non-continuation user/subsystem
   * storage. ARM also runs kernel continuations on MSP, so the old PSP page
   * can be freed immediately. */
#if defined(__ia16__)
  if (old_stack_id != PAGE_ID_INVALID) page_free(old_stack_id);
#else
  if (old_stack_id != PAGE_ID_INVALID) {
    void *os = page_to_ptr(old_stack_id);
    proc_release_stack_page(&os);
  }
#endif

  /* Free old user pages only if we owned them */
  if (owns_pages) {
    exec_snapshot_release_owned_segments(exec_snapshot);
    exec_snapshot_release_tracked_pages(exec_snapshot);
    if (old_user_stack) proc_release_stack_page(&old_user_stack);
  } else if (current->vfork_parent) {
    /* vfork child execve: free private tracked pages but not the shared
     * parent pages.  In the no-copy vfork model old_user_stack always
     * equals the parent's user_stack_page, so it's never freed here. */
    exec_snapshot_release_private_tracked_pages(
        exec_snapshot, current->vfork_parent->user_pages);
  }
  page_free(exec_snapshot);

  /* Unblock vfork parent — we have our own pages now */
  if (current->vfork_parent) {
    uint32_t saved = spin_lock_irqsave(SPIN_PROC);
    proc_wake_blocked_locked(current->vfork_parent);
    current->vfork_parent = NULL;
    spin_unlock_irqrestore(SPIN_PROC, saved);
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

long sys_set_tid_address(uintptr_t tidptr) {
  if (tidptr == 0u) {
    current->clear_child_tid = user_page_ref_invalid();
    return (long)current->pid;
  }
  if (proc_user_ptr_to_page_ref(current, tidptr, &current->clear_child_tid) < 0)
    return -(long)EFAULT;
  return (long)current->pid;
}

/* ── sys_uname ────────────────────────────────────────────────────────────────
 */

/* UTS_LEN and struct utsname layout come from common/utsname.h.
 * sys_uname writes fields by offset (no struct-access) so the kernel
 * doesn't need to instantiate the struct. */

static int sys_uname_copy_field(uintptr_t buf_ptr, uint16_t field_index,
                                const char *value) {
  char field[UTS_LEN];

  __builtin_memset(field, 0, sizeof(field));
  if (value) {
    for (int i = 0; value[i] && i < UTS_LEN - 1; i++) field[i] = value[i];
  }
  return sys_copy_to_user(buf_ptr + (uintptr_t)(field_index * UTS_LEN), field,
                          sizeof(field));
}

long sys_uname(uintptr_t buf_ptr) {
  if (buf_ptr == 0u) return -(long)EINVAL;

  if (sys_uname_copy_field(buf_ptr, 0u, PPAP_SYSNAME) < 0) return -(long)EFAULT;
  if (sys_uname_copy_field(buf_ptr, 1u, target_name()) < 0)
    return -(long)EFAULT;
  if (sys_uname_copy_field(buf_ptr, 2u, PPAP_VERSION) < 0) return -(long)EFAULT;
  if (sys_uname_copy_field(buf_ptr, 3u, "#1 PPAP") < 0) return -(long)EFAULT;
  if (sys_uname_copy_field(buf_ptr, 4u, PPAP_ARCH_NAME) < 0)
    return -(long)EFAULT;
  if (sys_uname_copy_field(buf_ptr, 5u, NULL) < 0) return -(long)EFAULT;
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
  struct {
    long ru_utime[2];
    long ru_stime[2];
    long stats[30];
  } rusage;
  long ret;

  ret = sys_waitpid(pid, status_ptr, options);
  if (ret < 0) return ret;
  if (rusage_ptr != 0u) {
    __builtin_memset(&rusage, 0, sizeof(rusage));
    if (sys_copy_to_user(rusage_ptr, &rusage, sizeof(rusage)) < 0)
      return -(long)EFAULT;
  }
  return ret;
}
