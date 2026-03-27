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

#include "exec.h"
#include "kernel/endian.h"
#include "kernel/common/errno.h"
#include "kernel/mm/page.h"
#include "kernel/subsys/subsys.h"
#include "loader.h"
#if !defined(__m68k__)
#include "h68k_emu.h"
#else
#include "kernel/signal/signal.h"
#include "kernel/subsys/human68k_loader.h"

/* PMB size for native path */
#define X68K_PMB_SIZE 0x100
#endif

/* ── Detection ─────────────────────────────────────────────────────────── */

static int r_detect(const uint8_t *file_buf, uint32_t file_size,
                    const char *path) {
  /* R-format has no magic — detect by ".r" or ".R" extension */
  if (file_size < 2) return 0;

  /* Must NOT be X-format (HU magic) or ELF */
  if (file_buf[0] == 0x48 && file_buf[1] == 0x55) return 0;
  if (file_buf[0] == 0x7F && file_buf[1] == 'E') return 0;

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

/* ── Loader ────────────────────────────────────────────────────────────── */

static int r_load(pcb_t *p, const uint8_t *file_buf, uint32_t file_size,
                  const cpu_ops_t *cpu_ops, void *cpu_state,
                  const char *const *argv, uint32_t flags) {
  (void)flags;
  (void)cpu_ops;
  (void)cpu_state;

  if (file_size == 0) return -(int)ENOEXEC;

#if !defined(__m68k__) && defined(PPAP_ENABLE_ECPU_M68K)
  /* ── Emulated path (requires m68k eCPU) ─────────────────────────────── */

  uint32_t min_pages = (X68K_PMB_SIZE + file_size + PAGE_SIZE - 1u) / PAGE_SIZE;
  if (min_pages > H68K_EMU_MEM_PAGES_MAX) return -(int)ENOMEM;

  p->stack_page = page_alloc();
  if (!p->stack_page) return -(int)ENOMEM;

  uint32_t min_total_pages = min_pages + 1u; /* +1 page for emu state */
  uint32_t total_pages = USER_PAGES_MAX;
  uint8_t *mem_base = NULL;
  while (total_pages >= min_total_pages) {
    mem_base = alloc_contiguous(total_pages);
    if (mem_base) break;
    if (total_pages == min_total_pages) break;
    total_pages--;
  }
  if (!mem_base) {
    page_free(p->stack_page);
    p->stack_page = NULL;
    return -(int)ENOMEM;
  }

  uint32_t emu_mem_pages = total_pages - 1u;
  uint32_t emu_mem_size = emu_mem_pages * PAGE_SIZE;
  uint8_t *emu_mem = mem_base;
  h68k_emu_exec_state_t *st =
      (h68k_emu_exec_state_t *)(mem_base + emu_mem_pages * PAGE_SIZE);

  for (uint32_t i = 0; i < total_pages; i++)
    proc_track_page(p, i, mem_base + i * PAGE_SIZE);

  memset(st, 0, sizeof(*st));
  ecpu_m68k_ops.init((cpu_state_t *)&st->m68k, emu_mem, emu_mem_size);
  ecpu_m68k_ops.set_trap_handler((cpu_state_t *)&st->m68k,
                                 h68k_emu_trap_handler, st);

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

  st->m68k.pc = X68K_PMB_SIZE;
  st->m68k.a[0] = 0x00000000u;
  st->m68k.a[1] = emu_mem_size;
  st->m68k.a[2] = 0x0000006Cu;
  st->m68k.a[3] = 0xFFFFFFFFu;
  st->m68k.a[4] = X68K_PMB_SIZE;
  st->m68k.a[7] = emu_mem_size;

  p->subsys = SUBSYS_PPAP;
  p->subsys_data = st;
  proc_setup_stack(p, h68k_emu_run_process, 0);

  (void)argv;
  return 0;

#else
  /* ── Native m68k path ──────────────────────────────────────────────── */

  uint32_t min_bytes = X68K_PMB_SIZE + file_size;
  uint32_t min_pages = (min_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

  if (min_pages > USER_PAGES_MAX) return -(int)ENOMEM;

  void *stack = page_alloc();
  if (!stack) return -(int)ENOMEM;
  p->stack_page = stack;

  uint32_t n_pages = USER_PAGES_MAX;
  uint8_t *base = NULL;
  while (n_pages >= min_pages) {
    base = alloc_contiguous(n_pages);
    if (base) break;
    n_pages--;
  }
  if (!base) {
    page_free(stack);
    p->stack_page = NULL;
    return -(int)ENOMEM;
  }

  uint32_t total_bytes = n_pages * PAGE_SIZE;

  for (uint32_t i = 0; i < n_pages; i++)
    proc_track_page(p, i, base + i * PAGE_SIZE);

  /* Zero PMB, copy file image */
  memset(base, 0, X68K_PMB_SIZE);
  uint8_t *image_dst = base + X68K_PMB_SIZE;
  memcpy(image_dst, file_buf, file_size);

  /* Zero remaining space after image (acts as BSS + stack area) */
  if (X68K_PMB_SIZE + file_size < total_bytes)
    memset(image_dst + file_size, 0, total_bytes - X68K_PMB_SIZE - file_size);

  const char *path = (argv && argv[0]) ? argv[0] : "";
  x68k_setup_pmb(base, total_bytes, file_size, path);

  /* Set up entry point (no relocation needed) */
  uint32_t entry = (uint32_t)(uintptr_t)image_dst;
  proc_setup_stack(p, (void (*)(void))(uintptr_t)entry, 0);

#if defined(__m68k__)
  p->usp = (uint32_t)(uintptr_t)(base + total_bytes);
#endif

  x68k_setup_registers(p->sp, (uint32_t)(uintptr_t)base,
                       (uint32_t)(uintptr_t)(base + total_bytes),
                       (uint32_t)(uintptr_t)(base + 0x6C),
                       (uint32_t)(uintptr_t)image_dst);

  p->subsys = SUBSYS_HUMAN68K;
  {
    const subsys_ops_t *ops = subsys_ops_table[SUBSYS_HUMAN68K];
    if (ops && ops->on_init) ops->on_init(p);
  }

  (void)argv;
  return 0;
#endif
}

/* ── Loader registration ───────────────────────────────────────────────── */

const loader_t r_loader = {
    .name = "r68k",
    .detect = r_detect,
    .load = r_load,
    .required_arch_id = CPU_ARCH_M68K,
    .xip = 0,
};
