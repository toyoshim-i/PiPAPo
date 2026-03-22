/*
 * m68k_emu_loader.c — m68k ELF binary loader for cross-arch emulation
 *
 * Detects m68k ELF binaries on non-m68k hosts, allocates emulated
 * memory, loads PT_LOAD segments, and sets up the eCPU-m68k state
 * for execution via the PPAP cross-arch personality.
 */

#include <string.h>

#include "elf.h"
#include "exec.h"
#include "kernel/cpu/ecpu_m68k.h"
#include "kernel/endian.h"
#include "kernel/errno.h"
#include "kernel/mm/page.h"
#include "kernel/subsys/ppap_m68k_bridge.h"
#include "kernel/subsys/subsys.h"
#include "loader.h"

/* Preferred emulated memory size — the loader will try this first,
 * then fall back to whatever contiguous pages are available (minimum
 * 64 KB = 16 pages).  Busybox needs ~250 KB for typical applets. */
#ifndef M68K_EMU_MEM_PREFERRED
#define M68K_EMU_MEM_PREFERRED (1 << 20) /* 1 MB default */
#endif
#define M68K_EMU_MEM_MIN (64u * 1024u) /* 64 KB absolute minimum */

/* ── Detection ─────────────────────────────────────────────────────────── */

static int m68k_emu_detect(const uint8_t *file_buf, uint32_t file_size,
                           const char *path) {
  (void)path;

#if defined(__m68k__)
  /* Native m68k — no emulation needed */
  (void)file_buf;
  (void)file_size;
  return 0;
#else
  if (file_size < sizeof(elf32_ehdr_t)) return 0;

  const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)file_buf;

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

/* ── Loader ────────────────────────────────────────────────────────────── */

static int m68k_emu_load(pcb_t *p, const uint8_t *file_buf, uint32_t file_size,
                         const cpu_ops_t *cpu_ops, void *cpu_state,
                         const char *const *argv) {
  (void)cpu_ops;
  (void)cpu_state;

  const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)file_buf;

  int argc = 0;
  if (argv) {
    while (argv[argc] != NULL) {
      argc++;
      if (argc > EXEC_ARGV_MAX) return -(int)E2BIG;
    }
  }
  if (argc == 0) argc = 1;

  /* ── 1. Allocate emulated memory + state struct ────────────────────── */
  uint32_t state_pages =
      (sizeof(ppap_m68k_exec_state_t) + PAGE_SIZE - 1) / PAGE_SIZE;

  /* Try preferred size first, then halve until we fit or hit minimum.
   * Reserve a few pages (4) for the process stack and kernel overhead. */
  uint32_t emu_mem_pages = M68K_EMU_MEM_PREFERRED / PAGE_SIZE;
  uint32_t min_emu_pages = M68K_EMU_MEM_MIN / PAGE_SIZE;
  uint8_t *mem_base = NULL;
  uint32_t total_pages;

  while (emu_mem_pages >= min_emu_pages) {
    total_pages = emu_mem_pages + state_pages;
    if (total_pages + 4 <= page_free_count()) {
      mem_base = alloc_contiguous(total_pages);
      if (mem_base) break;
    }
    emu_mem_pages /= 2;
  }
  if (!mem_base) return -(int)ENOMEM;

  uint32_t emu_mem_size = emu_mem_pages * PAGE_SIZE;
  uint8_t *emu_mem = mem_base;
  ppap_m68k_exec_state_t *state =
      (ppap_m68k_exec_state_t *)(mem_base + emu_mem_pages * PAGE_SIZE);

  /* Store pages for cleanup on exit */
  for (uint32_t i = 0; i < total_pages && i < USER_PAGES_MAX; i++)
    p->user_pages[i] = mem_base + i * PAGE_SIZE;

  /* ── 2. Allocate stack page ────────────────────────────────────────── */
  void *stack = page_alloc();
  if (!stack) {
    for (uint32_t i = 0; i < total_pages; i++)
      page_free(mem_base + i * PAGE_SIZE);
    return -(int)ENOMEM;
  }
  p->stack_page = stack;

  /* ── 3. Initialize m68k emulator ───────────────────────────────────── */
  memset(state, 0, sizeof(*state));
  ecpu_m68k_ops.init((cpu_state_t *)&state->m68k, emu_mem, emu_mem_size);

  /* Set trap handler for PPAP syscall interception */
  ecpu_m68k_ops.set_trap_handler((cpu_state_t *)&state->m68k,
                                 ppap_m68k_trap_handler, NULL);

  /* ── 4. Load PT_LOAD segments into emulated memory ─────────────────── */
  uint32_t phoff = be32_load(&ehdr->e_phoff);
  uint16_t phnum = be16_load(&ehdr->e_phnum);
  uint16_t phentsize = be16_load(&ehdr->e_phentsize);

  for (uint16_t i = 0; i < phnum; i++) {
    const uint8_t *ph = file_buf + phoff + i * phentsize;
    uint32_t p_type = be32_load(ph + 0);
    uint32_t p_offset = be32_load(ph + 4);
    uint32_t p_vaddr = be32_load(ph + 8);
    uint32_t p_filesz = be32_load(ph + 16);
    uint32_t p_memsz = be32_load(ph + 20);

    if (p_type != PT_LOAD) continue;

    /* Bounds check */
    if (p_vaddr + p_memsz > emu_mem_size) continue;
    if (p_offset + p_filesz > file_size) continue;

    /* Copy file data — already big-endian, m68k memory is big-endian */
    if (p_filesz > 0) memcpy(emu_mem + p_vaddr, file_buf + p_offset, p_filesz);

    /* Zero BSS */
    if (p_memsz > p_filesz)
      memset(emu_mem + p_vaddr + p_filesz, 0, p_memsz - p_filesz);
  }

  /* ── 5. Set entry point ────────────────────────────────────────────── */
  uint32_t entry = be32_load(&ehdr->e_entry);
  state->m68k.pc = entry;

  /* ── 6. Set up user stack with argc/argv (big-endian) ──────────────── */
  {
    uint32_t sp = emu_mem_size;

    /* Copy argument strings to stack */
    uint32_t str_addrs[EXEC_ARGV_MAX];
    if (argv && argv[0]) {
      for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        sp -= len;
        memcpy(emu_mem + sp, argv[i], len);
        str_addrs[i] = sp;
      }
    } else {
      /* Default: use path as argv[0] */
      const char *path = "";
      uint32_t len = (uint32_t)strlen(path) + 1;
      sp -= len;
      memcpy(emu_mem + sp, path, len);
      str_addrs[0] = sp;
    }

    /* Align to 4 bytes */
    sp &= ~3u;

    /* envp[0] = NULL */
    sp -= 4;
    m68k_write32(&state->m68k, sp, 0);

    /* argv[argc] = NULL */
    sp -= 4;
    m68k_write32(&state->m68k, sp, 0);

    /* argv[argc-1..0] */
    for (int i = argc - 1; i >= 0; i--) {
      sp -= 4;
      m68k_write32(&state->m68k, sp, str_addrs[i]);
    }

    /* argc */
    sp -= 4;
    m68k_write32(&state->m68k, sp, (uint32_t)argc);

    state->m68k.a[7] = sp;
  }

  /* ── 7. Set supervisor mode for kernel execution ───────────────────── */
  state->m68k.sr = M68K_SR_S;

  /* ── 8. Tag as PPAP process with eCPU m68k state ───────────────────── */
  p->subsys = SUBSYS_PPAP;
  p->subsys_data = state;

  /* ── 9. Set kernel-mode entry point ────────────────────────────────── */
  proc_setup_stack(p, ppap_m68k_run_process, 0);

  return 0;
}

/* ── Loader registration ───────────────────────────────────────────────── */

const loader_t m68k_emu_loader = {
    .name = "m68k_emu",
    .detect = m68k_emu_detect,
    .load = m68k_emu_load,
    .required_arch_id = CPU_ARCH_M68K,
    .xip = 0,
};
