/*
 * m68k_emu_loader.c — m68k ELF binary loader for cross-arch emulation
 *
 * Detects m68k ELF binaries on non-m68k hosts, allocates emulated
 * memory, initializes the m68k emulator, and delegates image loading
 * (PT_LOAD segments, entry point, argc/argv stack) to ppap_m68k_host.c.
 */

#include <string.h>

#include "common/errno.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/cpu/ecpu_m68k.h"
#include "kernel/core/endian.h"
#include "kernel/core/exec/elf.h"
#include "kernel/core/exec/exec.h"
#include "kernel/core/exec/loader.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/mm/page.h"
#include "kernel/core/subsys/ppap/ppap_m68k_bridge.h"
#include "kernel/core/subsys/ppap/ppap_m68k_host.h"
#include "kernel/core/subsys/subsys.h"

/* Preferred emulated memory size — the loader will try this first,
 * then fall back to whatever contiguous pages are available (minimum
 * 64 KB = 16 pages).  Busybox needs ~250 KB for typical applets. */
#ifndef M68K_EMU_MEM_PREFERRED
#define M68K_EMU_MEM_PREFERRED (1 << 20) /* 1 MB default */
#endif
#define M68K_EMU_MEM_MIN (64u * 1024u) /* 64 KB absolute minimum */

/* ── Detection ─────────────────────────────────────────────────────────── */

static int m68k_emu_detect(const uint8_t *header, uint32_t header_len,
                           uint32_t file_size, const char *path) {
  (void)path;
  (void)file_size;

#if defined(__m68k__)
  /* Native m68k — no emulation needed */
  (void)header;
  (void)header_len;
  return 0;
#else
  if (header_len < sizeof(elf32_ehdr_t)) return 0;

  const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)header;

  /* Check for valid ELF */
  if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
      ehdr->e_ident[EI_MAG2] != ELFMAG2 || ehdr->e_ident[EI_MAG3] != ELFMAG3)
    return 0;
  if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) return 0;
  if (ehdr->e_ident[EI_DATA] != ELFDATA2MSB) return 0;

  /* Read e_machine — it's big-endian in the file */
  const uint8_t *raw = (const uint8_t *)&ehdr->e_machine;
  uint16_t machine = ((uint16_t)raw[0] << 8) | raw[1];
  return machine == EM_68K;
#endif
}

static int m68k_emu_alloc_region(proc_image_segment_t *seg,
                                 uint32_t preferred_pages, uint32_t min_pages) {
  if (!seg || min_pages == 0 || preferred_pages < min_pages)
    return -(int)ENOMEM;

  uint32_t total_pages = preferred_pages;
  while (total_pages >= min_pages) {
    if (mem_region_alloc(seg, PPAP_MEM_RAM_DATA, total_pages * PAGE_SIZE,
                         PROC_IMAGE_SEG_WRITABLE | PROC_IMAGE_SEG_OWNED) == 0) {
      return (int)total_pages;
    }
    if (total_pages == min_pages) break;
    total_pages /= 2u;
    if (total_pages < min_pages) total_pages = min_pages;
  }

  *seg = (proc_image_segment_t){0};
  return -(int)ENOMEM;
}

/* ── Loader ────────────────────────────────────────────────────────────── */

static int m68k_emu_load(pcb_t *p, const uint8_t *file_buf, uint32_t file_size,
                         const cpu_ops_t *cpu_ops, void *cpu_state,
                         const char *const *argv, uint32_t flags) {
  (void)flags;
  (void)cpu_ops;
  (void)cpu_state;

  /* ── 1. Allocate emulated memory + state struct ────────────────────── */
  uint32_t state_pages =
      (sizeof(ppap_m68k_exec_state_t) + PAGE_SIZE - 1) / PAGE_SIZE;
  proc_image_segment_t data_region = {0};
  proc_image_segment_t stack_region = {0};

  uint32_t emu_mem_pages = M68K_EMU_MEM_PREFERRED / PAGE_SIZE;
  uint32_t min_emu_pages = M68K_EMU_MEM_MIN / PAGE_SIZE;
  int total_pages = m68k_emu_alloc_region(
      &data_region, emu_mem_pages + state_pages, min_emu_pages + state_pages);
  if (total_pages < 0) return total_pages;

  emu_mem_pages = (uint32_t)total_pages - state_pages;
  uint32_t emu_mem_size = emu_mem_pages * PAGE_SIZE;
  uint8_t *emu_mem = (uint8_t *)data_region.base;
  ppap_m68k_exec_state_t *state =
      (ppap_m68k_exec_state_t *)(emu_mem + emu_mem_pages * PAGE_SIZE);

  /* ── 2. Allocate stack page ────────────────────────────────────────── */
  if (mem_region_alloc(&stack_region, PPAP_MEM_RAM_STACK, PAGE_SIZE,
                       PROC_IMAGE_SEG_WRITABLE | PROC_IMAGE_SEG_OWNED) < 0) {
    mem_region_free(&data_region);
    return -(int)ENOMEM;
  }
  p->stack_page_id = mem_region_ptr_to_page(stack_region.base);
  p->image.stack = stack_region;

  /* ── 3. Initialize m68k emulator ───────────────────────────────────── */
  memset(state, 0, sizeof(*state));
  ecpu_m68k_ops.init((cpu_state_t *)&state->m68k, emu_mem, emu_mem_size);
  ecpu_m68k_ops.set_trap_handler((cpu_state_t *)&state->m68k,
                                 ppap_m68k_trap_handler, NULL);

  /* ── 4. Load ELF image (PT_LOAD segments + user stack) ─────────────── */
  uint32_t entry = 0;
  int rc = ppap_m68k_load_elf(&state->m68k, emu_mem, emu_mem_size, file_buf,
                              file_size, argv, &entry);
  if (rc < 0) {
    mem_region_free(&stack_region);
    mem_region_free(&data_region);
    return rc;
  }

  p->image.data = data_region;
  p->image.entry = entry;

  /* ── 5. Set supervisor mode for kernel execution ───────────────────── */
  state->m68k.sr = M68K_SR_S;

  /* ── 6. Tag as PPAP process with eCPU m68k state ───────────────────── */
  p->subsys = SUBSYS_PPAP;
  p->subsys_data = state;

  /* ── 7. Set kernel-mode entry point ────────────────────────────────── */
  proc_setup_stack(p, ppap_m68k_run_process, 0);

  return 0;
}

static int m68k_emu_load_vn(pcb_t *p, vnode_t *vn, uint32_t file_size,
                            const cpu_ops_t *cpu_ops, void *cpu_state,
                            const char *const *argv, uint32_t flags) {
  /* Emulated m68k runs on ARM/RV/Xtensa (all flat-pointer arches), so
   * staging is safe. */
  proc_image_segment_t staging = {0};
  if (mem_region_alloc(&staging, PPAP_MEM_RAM_DATA, file_size,
                       PROC_IMAGE_SEG_WRITABLE) < 0)
    return -(int)ENOMEM;

  uintptr_t addr = (uintptr_t)staging.base;
  page_id_t page = (page_id_t)(addr / PAGE_SIZE);
  uint16_t page_off = (uint16_t)(addr & (PAGE_SIZE - 1u));
  long n = mod_vfs.vnode_read(vn, page, page_off, file_size, 0);
  if (n < 0 || (uint32_t)n != file_size) {
    mem_region_free(&staging);
    return (n < 0) ? (int)n : -(int)ENOEXEC;
  }

  int rc = m68k_emu_load(p, (const uint8_t *)staging.base, file_size, cpu_ops,
                         cpu_state, argv, flags);
  mem_region_free(&staging);
  return rc;
}

/* ── Loader registration ───────────────────────────────────────────────── */

const loader_t m68k_emu_loader = {
    .name = "m68k_emu",
    .detect = m68k_emu_detect,
    .load_vn = m68k_emu_load_vn,
    .required_arch_id = CPU_ARCH_M68K,
};
