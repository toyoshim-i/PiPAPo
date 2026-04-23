/*
 * r_loader.c — Human68k R-format binary loader
 *
 * Detects R-format (.r) binaries by extension (no magic number) and
 * loads them as raw flat binaries with no header or relocation.
 * On native m68k, runs directly in user mode.  On other architectures,
 * runs via the m68k emulator.
 *
 * See docs/subsystems/human68k.md for the full design.
 */

#include <string.h>

#include "common/errno.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/endian.h"
#include "kernel/core/exec/exec.h"
#include "kernel/core/exec/exec_args.h"
#include "kernel/core/exec/loader.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/mm/page.h"
#include "kernel/core/subsys/subsys.h"
#if !defined(__m68k__)
#include "kernel/core/subsys/human68k/m68k_emu.h"
#else
#include "kernel/core/signal/signal.h"
#include "kernel/core/subsys/human68k/human68k_host.h"

/* PMB size for native path */
#define X68K_PMB_SIZE 0x100
#endif

/* ── Detection ─────────────────────────────────────────────────────────── */

static int r_detect(const uint8_t *header, uint32_t header_len,
                    uint32_t file_size, const char *path) {
  /* R-format has no magic — detect by ".r" or ".R" extension */
  if (file_size < 2) return 0;

  /* Must NOT be X-format (HU magic) or ELF */
  if (header_len >= 2) {
    if (header[0] == 0x48 && header[1] == 0x55) return 0;
    if (header[0] == 0x7F && header[1] == 'E') return 0;
  }

  /* Check extension */
  const char *dot = NULL;
  for (const char *s = path; *s; s++) {
    if (*s == '.')
      dot = s;
    else if (*s == '/')
      dot = NULL;
  }
  if (!dot) return 0;
  return (dot[1] == 'r' || dot[1] == 'R') && dot[2] == '\0';
}

static int r68k_alloc_largest_image_region(proc_image_segment_t *seg,
                                           uint32_t min_pages) {
  if (!seg || min_pages == 0 || min_pages > USER_PAGES_MAX) return -(int)ENOMEM;

  for (uint32_t total_pages = USER_PAGES_MAX; total_pages >= min_pages;
       total_pages--) {
    if (mem_region_alloc(seg, PPAP_MEM_RAM_DATA, total_pages * PAGE_SIZE,
                         PROC_IMAGE_SEG_WRITABLE) == 0)
      return (int)total_pages;
    if (total_pages == min_pages) break;
  }

  *seg = (proc_image_segment_t){0};
  return -(int)ENOMEM;
}

/* ── Loader ────────────────────────────────────────────────────────────── */

static int r_load(pcb_t *p, vnode_t *vn, uint32_t file_size,
                  const cpu_ops_t *cpu_ops, void *cpu_state,
                  const exec_args_t *args, uint32_t flags) {
  (void)flags;
  (void)cpu_ops;
  (void)cpu_state;

  if (file_size == 0) return -(int)ENOEXEC;

  /* r_loader runs on m68k (native or emulated), so staging is safe. */
  proc_image_segment_t staging = {0};
  if (mem_region_alloc(&staging, PPAP_MEM_RAM_DATA, file_size,
                       PROC_IMAGE_SEG_WRITABLE) < 0)
    return -(int)ENOMEM;
  {
    uintptr_t addr = (uintptr_t)staging.base;
    page_id_t page = (page_id_t)(addr / PAGE_SIZE);
    uint16_t page_off = (uint16_t)(addr & (PAGE_SIZE - 1u));
    long n = mod_vfs.vnode_read(vn, page, page_off, file_size, 0);
    if (n < 0 || (uint32_t)n != file_size) {
      mem_region_free(&staging);
      return (n < 0) ? (int)n : -(int)ENOEXEC;
    }
  }
  const uint8_t *file_buf = (const uint8_t *)staging.base;

  proc_image_segment_t stack_region = {0};
  proc_image_segment_t image_region = {0};

  if (mem_region_alloc(&stack_region, PPAP_MEM_RAM_STACK, PAGE_SIZE,
                       PROC_IMAGE_SEG_WRITABLE | PROC_IMAGE_SEG_OWNED) < 0) {
    mem_region_free(&staging);
    return -(int)ENOMEM;
  }
  p->stack_page_id = mem_region_ptr_to_page(stack_region.base);
  p->image.stack = stack_region;

#if !defined(__m68k__) && defined(PPAP_ENABLE_ECPU_M68K)
  /* ── Emulated path (requires m68k eCPU) ─────────────────────────────── */

  uint32_t min_pages = (X68K_PMB_SIZE + file_size + PAGE_SIZE - 1u) / PAGE_SIZE;
  if (min_pages > H68K_EMU_MEM_PAGES_MAX) {
    mem_region_free(&stack_region);
    mem_region_free(&staging);
    p->stack_page_id = PAGE_ID_INVALID;
    p->image.stack = (proc_image_segment_t){0};
    return -(int)ENOMEM;
  }

  uint32_t min_total_pages = min_pages + 1u; /* +1 page for emu state */
  int total_pages =
      r68k_alloc_largest_image_region(&image_region, min_total_pages);
  if (total_pages < 0) {
    mem_region_free(&stack_region);
    mem_region_free(&staging);
    p->stack_page_id = PAGE_ID_INVALID;
    p->image.stack = (proc_image_segment_t){0};
    return total_pages;
  }

  if (proc_track_page_range(p, 0, mem_region_ptr_to_page(image_region.base),
                            image_region.size / PAGE_SIZE) < 0) {
    mem_region_free(&image_region);
    mem_region_free(&stack_region);
    mem_region_free(&staging);
    p->stack_page_id = PAGE_ID_INVALID;
    p->image.stack = (proc_image_segment_t){0};
    return -(int)ENOMEM;
  }

  uint8_t *mem_base = (uint8_t *)image_region.base;
  uint32_t emu_mem_pages = (uint32_t)total_pages - 1u;
  uint32_t emu_mem_size = emu_mem_pages * PAGE_SIZE;
  uint8_t *emu_mem = mem_base;
  m68k_emu_exec_state_t *st =
      (m68k_emu_exec_state_t *)(mem_base + emu_mem_pages * PAGE_SIZE);

  memset(st, 0, sizeof(*st));
  ecpu_m68k_ops.init((cpu_state_t *)&st->m68k, emu_mem, emu_mem_size);
  ecpu_m68k_ops.set_trap_handler((cpu_state_t *)&st->m68k,
                                 m68k_emu_trap_handler, st);

  memset(emu_mem, 0, emu_mem_size);
  {
    uint32_t guest_end = emu_mem_size;
    be32_store(emu_mem + 0x00, 0);
    be32_store(emu_mem + 0x04, 0);
    be32_store(emu_mem + 0x08, guest_end);
    be32_store(emu_mem + 0x0C, 0);
    be32_store(emu_mem + 0x10, 0xFFFFFFFFu);
    be32_store(emu_mem + 0x20, 0x0000006Cu);
    emu_mem[0x24] = 0x07;
    be32_store(emu_mem + 0x30, X68K_PMB_SIZE + file_size);
    be32_store(emu_mem + 0x34, X68K_PMB_SIZE + file_size);
    be32_store(emu_mem + 0x38, guest_end);

    st->heap_next = (X68K_PMB_SIZE + file_size + 3u) & ~3u;
    st->block_end = guest_end;
  }

  memcpy(emu_mem + X68K_PMB_SIZE, file_buf, file_size);
  mem_region_free(&staging);

  st->m68k.pc = X68K_PMB_SIZE;
  st->m68k.a[0] = 0x00000000u;
  st->m68k.a[1] = emu_mem_size;
  st->m68k.a[2] = 0x0000006Cu;
  st->m68k.a[3] = 0xFFFFFFFFu;
  st->m68k.a[4] = X68K_PMB_SIZE;
  st->m68k.a[7] = emu_mem_size;

  p->image.text =
      proc_image_segment_make(emu_mem + X68K_PMB_SIZE, file_size,
                              PPAP_MEM_RAM_TEXT, PROC_IMAGE_SEG_EXECUTABLE);
  p->image.data = image_region;
  p->image.entry = X68K_PMB_SIZE;
  p->subsys = SUBSYS_PPAP;
  p->subsys_data = st;
  proc_setup_stack(p, m68k_emu_run_process, 0);

  (void)args;
  return 0;

#else
  /* ── Native m68k path ──────────────────────────────────────────────── */

  uint32_t min_bytes = X68K_PMB_SIZE + file_size;
  uint32_t min_pages = (min_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

  if (min_pages > USER_PAGES_MAX) {
    mem_region_free(&stack_region);
    mem_region_free(&staging);
    p->stack_page_id = PAGE_ID_INVALID;
    p->image.stack = (proc_image_segment_t){0};
    return -(int)ENOMEM;
  }
  int n_pages = r68k_alloc_largest_image_region(&image_region, min_pages);
  if (n_pages < 0) {
    mem_region_free(&stack_region);
    mem_region_free(&staging);
    p->stack_page_id = PAGE_ID_INVALID;
    p->image.stack = (proc_image_segment_t){0};
    return n_pages;
  }

  if (proc_track_page_range(p, 0, mem_region_ptr_to_page(image_region.base),
                            image_region.size / PAGE_SIZE) < 0) {
    mem_region_free(&image_region);
    mem_region_free(&stack_region);
    mem_region_free(&staging);
    p->stack_page_id = PAGE_ID_INVALID;
    p->image.stack = (proc_image_segment_t){0};
    return -(int)ENOMEM;
  }

  uint8_t *base = (uint8_t *)image_region.base;
  uint32_t total_bytes = (uint32_t)n_pages * PAGE_SIZE;

  /* Zero PMB, copy file image */
  memset(base, 0, X68K_PMB_SIZE);
  uint8_t *image_dst = base + X68K_PMB_SIZE;
  memcpy(image_dst, file_buf, file_size);
  mem_region_free(&staging);

  /* Zero remaining space after image (acts as BSS + stack area) */
  if (X68K_PMB_SIZE + file_size < total_bytes)
    memset(image_dst + file_size, 0, total_bytes - X68K_PMB_SIZE - file_size);

  char path[VFS_PATH_MAX];
  if (exec_args_path(args, path, sizeof(path)) < 0) path[0] = '\0';

  /* Build env block (separate page, tracked for exit cleanup). */
  page_id_t env_page = PAGE_ID_INVALID;
  uint32_t env_addr = 0xFFFFFFFFu;
  if (human68k_build_env(args, &env_page, &env_addr) < 0) {
    mem_region_free(&image_region);
    mem_region_free(&stack_region);
    p->stack_page_id = PAGE_ID_INVALID;
    p->image.stack = (proc_image_segment_t){0};
    return -(int)ENOMEM;
  }
  if (env_page != PAGE_ID_INVALID) {
    if (proc_track_page(p, USER_PAGES_MAX - 1, env_page) < 0) {
      mem_region_page_free(env_page);
      env_addr = 0xFFFFFFFFu;
    }
  }

  human68k_setup_pmb(base, total_bytes, file_size, path, env_addr);

  /* Set up entry point (no relocation needed) */
  uint32_t entry = (uint32_t)(uintptr_t)image_dst;
  proc_setup_stack(p, (void (*)(void))(uintptr_t)entry, 0);
  p->image.text = proc_image_segment_make(
      image_dst, file_size, PPAP_MEM_RAM_TEXT, PROC_IMAGE_SEG_EXECUTABLE);
  p->image.data = image_region;
  p->image.entry = entry;

#if defined(__m68k__)
  p->usp = (uint32_t)(uintptr_t)(base + total_bytes);
#endif

  human68k_setup_registers(p->sp, (uint32_t)(uintptr_t)base,
                           (uint32_t)(uintptr_t)(base + total_bytes),
                           (uint32_t)(uintptr_t)(base + 0x6C), env_addr,
                           (uint32_t)(uintptr_t)image_dst);

  p->subsys = SUBSYS_HUMAN68K;
  {
    const subsys_ops_t *ops = subsys_ops_table[SUBSYS_HUMAN68K];
    if (ops && ops->on_init) ops->on_init(p);
  }

  return 0;
#endif
}

/* ── Loader registration ───────────────────────────────────────────────── */

const loader_t r_loader = {
    .name = "r68k",
    .detect = r_detect,
    .load = r_load,
    .required_arch_id = CPU_ARCH_M68K,
};
