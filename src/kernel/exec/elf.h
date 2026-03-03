/*
 * elf.h — Minimal ELF32 definitions and parser API
 *
 * Provides type definitions for ELF32 headers and a small validation /
 * segment-extraction API used by execve() to load user-space binaries
 * from romfs XIP flash.
 *
 * PIC binaries (ET_EXEC compiled with -fPIC, linked at address 0) are
 * loaded via XIP from flash with GOT relocation into SRAM.
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
#define PT_DYNAMIC  2

/* ── Relocation types ───────────────────────────────────────────────────── */

#define R_ARM_RELATIVE  23      /* Adjust by load base (PIE relocation) */

/* ── Section types ──────────────────────────────────────────────────────── */

#define SHT_REL     9           /* Relocation entries (without addend) */

/* ── ELF32 relocation entry (8 bytes) ───────────────────────────────────── */

typedef struct {
    uint32_t r_offset;          /* address of the word to relocate */
    uint32_t r_info;            /* type + symbol index */
} elf32_rel_t;

#define ELF32_R_TYPE(i)  ((i) & 0xffu)

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

/* ── ELF32 section header (40 bytes) ────────────────────────────────────── */

typedef struct {
    uint32_t sh_name;           /* index into section name string table */
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;           /* virtual address in linked image */
    uint32_t sh_offset;         /* file offset */
    uint32_t sh_size;           /* section size in bytes */
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} elf32_shdr_t;

/* ── GOT location descriptor ────────────────────────────────────────────── */

typedef struct {
    uint32_t offset;    /* file offset of .got section */
    uint32_t addr;      /* link address (virtual address) of .got */
    uint32_t size;      /* size in bytes */
} elf_got_info_t;

/* ── Relocation table descriptor ───────────────────────────────────────── */

typedef struct {
    uint32_t offset;    /* file offset of .rel.dyn section */
    uint32_t size;      /* size in bytes (each entry = 8 bytes) */
} elf_rel_info_t;

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

/*
 * elf_find_got — locate the .got section in an ELF binary.
 *
 * Searches the section header table for a section named ".got" and fills
 * `out` with its file offset, link address, and size.
 *
 * Returns 0 on success, 1 if no .got section exists (binary has no global
 * data references), or -ENOEXEC if section headers are absent or malformed.
 */
int elf_find_got(const elf32_ehdr_t *ehdr, const uint8_t *file_base,
                 elf_got_info_t *out);

/*
 * elf_find_rel — locate the .rel.dyn section in an ELF binary.
 *
 * PIE binaries contain R_ARM_RELATIVE entries that tell the loader
 * which data-segment words hold addresses needing relocation (e.g.
 * function-pointer arrays like applet_main[]).
 *
 * Returns 0 on success, 1 if no .rel.dyn exists, -ENOEXEC on error.
 */
int elf_find_rel(const elf32_ehdr_t *ehdr, const uint8_t *file_base,
                 elf_rel_info_t *out);

#endif /* PPAP_EXEC_ELF_H */
