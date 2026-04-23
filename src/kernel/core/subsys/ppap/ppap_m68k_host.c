/*
 * ppap_m68k_host.c — PPAP-on-m68k-emulator image setup
 *
 * Loads a cross-arch m68k ELF binary into pre-allocated emulated
 * memory: PT_LOAD segments, entry point, and initial argc/argv/envp
 * stack layout.
 */

#include "kernel/core/subsys/ppap/ppap_m68k_host.h"

#include <string.h>

#include "common/errno.h"
#include "kernel/core/endian.h"
#include "kernel/core/exec/elf.h"
#include "kernel/core/exec/exec.h"
#include "kernel/core/exec/exec_args.h"

int ppap_m68k_load_elf(m68k_state_t *cpu, uint8_t *emu_mem,
                       uint32_t emu_mem_size, const uint8_t *file_buf,
                       uint32_t file_size, const exec_args_t *args,
                       uint32_t *entry_out) {
  const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)file_buf;

  int argc = args ? (int)args->argc : 0;
  if (argc == 0) argc = 1;

  /* ── Load PT_LOAD segments into emulated memory ────────────────────── */
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

  /* ── Set entry point ───────────────────────────────────────────────── */
  uint32_t entry = be32_load(&ehdr->e_entry);
  cpu->pc = entry;
  if (entry_out) *entry_out = entry;

  /* ── Build initial user stack (argc/argv/envp, big-endian) ─────────── */
  uint32_t sp = emu_mem_size;

  /* Copy argument strings */
  uint32_t str_addrs[EXEC_ARGV_MAX];
  if (args && args->argc > 0) {
    for (int i = argc - 1; i >= 0; i--) {
      uint16_t len = exec_args_argv_len(args, i);
      sp -= (uint32_t)len + 1u;
      exec_args_argv_copy(args, i, (char *)(emu_mem + sp),
                          (uint16_t)(len + 1u));
      str_addrs[i] = sp;
    }
  } else {
    /* Default: empty argv[0] */
    sp -= 1u;
    emu_mem[sp] = '\0';
    str_addrs[0] = sp;
  }

  /* Align to 4 bytes */
  sp &= ~3u;

  /* envp[0] = NULL */
  sp -= 4;
  m68k_write32(cpu, sp, 0);

  /* argv[argc] = NULL */
  sp -= 4;
  m68k_write32(cpu, sp, 0);

  /* argv[argc-1..0] */
  for (int i = argc - 1; i >= 0; i--) {
    sp -= 4;
    m68k_write32(cpu, sp, str_addrs[i]);
  }

  /* argc */
  sp -= 4;
  m68k_write32(cpu, sp, (uint32_t)argc);

  cpu->a[7] = sp;

  return 0;
}
