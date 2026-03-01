/*
 * elf.h — Minimal ELF32 definitions and parser API
 *
 * Provides type definitions for ELF32 headers and a small validation /
 * segment-extraction API used by execve() to load user-space binaries
 * from romfs XIP flash.
 *
 * Only static ELF32 ARM Thumb binaries are supported (no dynamic linking,
 * no relocation).  ET_DYN is accepted for future PIC binaries.
 */

#ifndef PPAP_EXEC_ELF_H
#define PPAP_EXEC_ELF_H

#include <stdint.h>

/* ── ELF identification indices ──────────────────────────────────────────── */

#define EI_NIDENT   16
#define EI_MAG0      0
#define EI_MAG1      1
#define EI_MAG2      2
#define EI_MAG3      3
#define EI_CLASS     4
#define EI_DATA      5

/* ── ELF magic ───────────────────────────────────────────────────────────── */

#define ELFMAG0     0x7f
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'

/* ── ELF class ───────────────────────────────────────────────────────────── */

#define ELFCLASS32  1

/* ── ELF data encoding ───────────────────────────────────────────────────── */

#define ELFDATA2LSB 1       /* little-endian */

/* ── ELF type ────────────────────────────────────────────────────────────── */

#define ET_EXEC     2       /* static executable */
#define ET_DYN      3       /* PIC / shared object */

/* ── ELF machine ─────────────────────────────────────────────────────────── */

#define EM_ARM      40

/* ── ARM ELF flags ───────────────────────────────────────────────────────── */

#define EF_ARM_EABI_VER5        0x05000000u
#define EF_ARM_ABI_FLOAT_SOFT   0x00000200u
#define EF_ARM_EABI_VER_MASK    0xFF000000u

/* ── Program header types ────────────────────────────────────────────────── */

#define PT_LOAD     1

/* ── Program header flags ────────────────────────────────────────────────── */

#define PF_X        0x1
#define PF_W        0x2
#define PF_R        0x4

/* ── ELF32 header (52 bytes) ─────────────────────────────────────────────── */

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;           /* entry point virtual address */
    uint32_t e_phoff;           /* program header table file offset */
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;           /* number of program headers */
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf32_ehdr_t;

/* ── ELF32 program header (32 bytes) ─────────────────────────────────────── */

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;          /* offset in file */
    uint32_t p_vaddr;           /* virtual address */
    uint32_t p_paddr;           /* physical address (unused) */
    uint32_t p_filesz;          /* size in file */
    uint32_t p_memsz;           /* size in memory (>= filesz; excess is BSS) */
    uint32_t p_flags;           /* PF_R | PF_W | PF_X */
    uint32_t p_align;
} elf32_phdr_t;

/* ── Parser API ──────────────────────────────────────────────────────────── */

/*
 * elf_validate — check that ehdr describes a valid ELF32 ARM executable.
 *
 * Checks: magic, class (ELF32), data (little-endian), machine (ARM),
 * type (ET_EXEC or ET_DYN), EABI version 5, and presence of program headers.
 *
 * Returns 0 on success, -ENOEXEC on any validation failure.
 */
int elf_validate(const elf32_ehdr_t *ehdr);

/*
 * elf_load_segments — extract PT_LOAD program headers.
 *
 * Iterates the program header table at file_base + ehdr->e_phoff and
 * copies each PT_LOAD entry into out[].  Non-LOAD segments are skipped.
 *
 * Returns the number of PT_LOAD segments found (>= 0), or -ENOEXEC if
 * the program header table is malformed.
 */
int elf_load_segments(const elf32_ehdr_t *ehdr, const uint8_t *file_base,
                      elf32_phdr_t *out, int max);

/*
 * elf_entry — return the entry point address from the ELF header.
 *
 * For ARM Thumb binaries the address has bit 0 set (Thumb bit).
 */
uint32_t elf_entry(const elf32_ehdr_t *ehdr);

#endif /* PPAP_EXEC_ELF_H */
