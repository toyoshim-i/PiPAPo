/*
 * elf.c — Minimal ELF32 parser for PPAP
 *
 * Validates ELF32 headers and extracts PT_LOAD segments.
 * Supports ARM (little-endian) and m68k (big-endian) targets.
 * Used by execve() to locate the entry point and loadable segments
 * of user-space binaries stored in romfs.
 *
 * Pure C — no hardware dependencies, no memory allocation.
 */

#include "kernel/core/exec/elf.h"

#include "common/errno.h"

static void copy_bytes(void *dst, const void *src, uint32_t len) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  for (uint32_t i = 0; i < len; i++) d[i] = s[i];
}

/* ── elf_validate ────────────────────────────────────────────────────────── */

int elf_validate(const elf32_ehdr_t *ehdr) {
  /* Magic: \x7fELF */
  if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
      ehdr->e_ident[EI_MAG2] != ELFMAG2 || ehdr->e_ident[EI_MAG3] != ELFMAG3)
    return -(int)ENOEXEC;

  /* Class: must be ELF32 */
  if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) return -(int)ENOEXEC;

    /* Architecture-specific checks */
#if defined(__m68k__)
  if (ehdr->e_ident[EI_DATA] != ELFDATA2MSB) return -(int)ENOEXEC;
  if (ehdr->e_machine != EM_68K) return -(int)ENOEXEC;
#elif defined(__xtensa__)
  if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) return -(int)ENOEXEC;
  if (ehdr->e_machine != EM_XTENSA) return -(int)ENOEXEC;
#elif defined(__riscv)
  if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) return -(int)ENOEXEC;
  if (ehdr->e_machine != EM_RISCV) return -(int)ENOEXEC;
#else
  if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) return -(int)ENOEXEC;
  if (ehdr->e_machine != EM_ARM) return -(int)ENOEXEC;
#endif

  /* Type: ET_EXEC (static) or ET_DYN (PIC) */
  if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) return -(int)ENOEXEC;

#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
  /* ARM EABI version 5 */
  if ((ehdr->e_flags & EF_ARM_EABI_VER_MASK) != EF_ARM_EABI_VER5)
    return -(int)ENOEXEC;
#endif

  /* Must have program headers with valid entry size */
  if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return -(int)ENOEXEC;
  if (ehdr->e_phentsize < sizeof(elf32_phdr_t)) return -(int)ENOEXEC;

  return 0;
}

/* ── elf_load_segments ───────────────────────────────────────────────────── */

int elf_load_segments(const elf32_ehdr_t *ehdr, const uint8_t *file_base,
                      elf32_phdr_t *out, int max, uint32_t file_size) {
  /* Validate program header table fits within the file */
  uint32_t ph_end = ehdr->e_phoff + (uint32_t)ehdr->e_phnum * ehdr->e_phentsize;
  if (ph_end < ehdr->e_phoff || ph_end > file_size) return -(int)ENOEXEC;

  const uint8_t *ph_table = file_base + ehdr->e_phoff;
  int count = 0;

  for (int i = 0; i < ehdr->e_phnum; i++) {
    const elf32_phdr_t *ph =
        (const elf32_phdr_t *)(ph_table + i * ehdr->e_phentsize);

    if (ph->p_type != PT_LOAD) continue;

    /* Validate segment file data fits within the file */
    if (ph->p_offset + ph->p_filesz < ph->p_offset ||
        ph->p_offset + ph->p_filesz > file_size)
      return -(int)ENOEXEC;

    if (count < max) {
      copy_bytes(&out[count], ph, sizeof(elf32_phdr_t));
    }
    count++;
  }

  return count;
}

/* ── elf_entry ───────────────────────────────────────────────────────────── */

uint32_t elf_entry(const elf32_ehdr_t *ehdr) { return ehdr->e_entry; }

/* ── elf_find_got ───────────────────────────────────────────────────────── */

static int str_eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

int elf_find_got(const elf32_ehdr_t *ehdr, const uint8_t *file_base,
                 elf_got_info_t *out, uint32_t file_size) {
  if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0)
    return 1; /* no section headers */
  if (ehdr->e_shstrndx == 0 || ehdr->e_shstrndx >= ehdr->e_shnum) return 1;

  /* Validate section header table fits within the file */
  uint32_t sh_end =
      ehdr->e_shoff + (uint32_t)ehdr->e_shnum * sizeof(elf32_shdr_t);
  if (sh_end < ehdr->e_shoff || sh_end > file_size) return 1;

  const elf32_shdr_t *sh_table =
      (const elf32_shdr_t *)(file_base + ehdr->e_shoff);

  /* Validate strtab offset */
  if (sh_table[ehdr->e_shstrndx].sh_offset >= file_size) return 1;

  const char *strtab =
      (const char *)(file_base + sh_table[ehdr->e_shstrndx].sh_offset);

  for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
    const char *name = strtab + sh_table[i].sh_name;
    if (str_eq(name, ".got")) {
      out->offset = sh_table[i].sh_offset;
      out->addr = sh_table[i].sh_addr;
      out->size = sh_table[i].sh_size;
      return 0;
    }
  }

  return 1; /* no .got section */
}

int elf_find_section(const elf32_ehdr_t *ehdr, const uint8_t *file_base,
                     const char *name, elf_got_info_t *out,
                     uint32_t file_size) {
  if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) return 1;
  if (ehdr->e_shstrndx == 0 || ehdr->e_shstrndx >= ehdr->e_shnum) return 1;

  uint32_t sh_end =
      ehdr->e_shoff + (uint32_t)ehdr->e_shnum * sizeof(elf32_shdr_t);
  if (sh_end < ehdr->e_shoff || sh_end > file_size) return 1;

  const elf32_shdr_t *sh_table =
      (const elf32_shdr_t *)(file_base + ehdr->e_shoff);

  if (sh_table[ehdr->e_shstrndx].sh_offset >= file_size) return 1;

  const char *strtab =
      (const char *)(file_base + sh_table[ehdr->e_shstrndx].sh_offset);

  for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
    const char *sname = strtab + sh_table[i].sh_name;
    if (str_eq(sname, name)) {
      out->offset = sh_table[i].sh_offset;
      out->addr = sh_table[i].sh_addr;
      out->size = sh_table[i].sh_size;
      return 0;
    }
  }
  return 1;
}

/* ── elf_find_rel ────────────────────────────────────────────────────────── */

int elf_find_rel(const elf32_ehdr_t *ehdr, const uint8_t *file_base,
                 elf_rel_info_t *out, uint32_t file_size) {
  if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) return 1;
  if (ehdr->e_shstrndx == 0 || ehdr->e_shstrndx >= ehdr->e_shnum) return 1;

  /* Validate section header table fits within the file */
  uint32_t sh_end =
      ehdr->e_shoff + (uint32_t)ehdr->e_shnum * sizeof(elf32_shdr_t);
  if (sh_end < ehdr->e_shoff || sh_end > file_size) return 1;

  const elf32_shdr_t *sh_table =
      (const elf32_shdr_t *)(file_base + ehdr->e_shoff);

  /* Validate strtab offset */
  if (sh_table[ehdr->e_shstrndx].sh_offset >= file_size) return 1;

  const char *strtab =
      (const char *)(file_base + sh_table[ehdr->e_shstrndx].sh_offset);

  for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
    const char *name = strtab + sh_table[i].sh_name;
    if (sh_table[i].sh_type == SHT_REL && str_eq(name, ".rel.dyn")) {
      out->offset = sh_table[i].sh_offset;
      out->size = sh_table[i].sh_size;
      out->has_addend = 0;
      return 0;
    }
    if (sh_table[i].sh_type == SHT_RELA && str_eq(name, ".rela.dyn")) {
      out->offset = sh_table[i].sh_offset;
      out->size = sh_table[i].sh_size;
      out->has_addend = 1;
      return 0;
    }
  }

  return 1; /* no .rel.dyn/.rela.dyn section */
}

/* ── elf_find_dynsym ─────────────────────────────────────────────────────── */

int elf_find_dynsym(const elf32_ehdr_t *ehdr, const uint8_t *file_base,
                    elf_dynsym_info_t *out, uint32_t file_size) {
  if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) return 1;

  uint32_t sh_end =
      ehdr->e_shoff + (uint32_t)ehdr->e_shnum * sizeof(elf32_shdr_t);
  if (sh_end < ehdr->e_shoff || sh_end > file_size) return 1;

  const elf32_shdr_t *sh_table =
      (const elf32_shdr_t *)(file_base + ehdr->e_shoff);

  for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
    if (sh_table[i].sh_type == SHT_DYNSYM) {
      out->offset = sh_table[i].sh_offset;
      out->count = sh_table[i].sh_size / sizeof(elf32_sym_t);
      return 0;
    }
  }

  return 1;
}
