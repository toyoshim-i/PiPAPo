/*
 * elf.c — Minimal ELF32 parser for PPAP
 *
 * Validates ELF32 ARM Thumb headers and extracts PT_LOAD segments.
 * Used by execve() (Phase 3 Step 3) to locate the entry point and
 * loadable segments of user-space binaries stored in romfs XIP flash.
 *
 * Pure C — no hardware dependencies, no memory allocation.
 */

#include "elf.h"
#include "kernel/errno.h"
#include <string.h>     /* memcmp */

/* ── elf_validate ────────────────────────────────────────────────────────── */

int elf_validate(const elf32_ehdr_t *ehdr)
{
    /* Magic: \x7fELF */
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3)
        return -(int)ENOEXEC;

    /* Class: must be ELF32 */
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32)
        return -(int)ENOEXEC;

    /* Data: must be little-endian */
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB)
        return -(int)ENOEXEC;

    /* Machine: must be ARM */
    if (ehdr->e_machine != EM_ARM)
        return -(int)ENOEXEC;

    /* Type: ET_EXEC (static) or ET_DYN (PIC) */
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
        return -(int)ENOEXEC;

    /* ARM EABI version 5 */
    if ((ehdr->e_flags & EF_ARM_EABI_VER_MASK) != EF_ARM_EABI_VER5)
        return -(int)ENOEXEC;

    /* Must have program headers */
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0)
        return -(int)ENOEXEC;

    return 0;
}

/* ── elf_load_segments ───────────────────────────────────────────────────── */

int elf_load_segments(const elf32_ehdr_t *ehdr, const uint8_t *file_base,
                      elf32_phdr_t *out, int max)
{
    const uint8_t *ph_table = file_base + ehdr->e_phoff;
    int count = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        const elf32_phdr_t *ph =
            (const elf32_phdr_t *)(ph_table + i * ehdr->e_phentsize);

        if (ph->p_type != PT_LOAD)
            continue;

        if (count < max) {
            memcpy(&out[count], ph, sizeof(elf32_phdr_t));
        }
        count++;
    }

    return count;
}

/* ── elf_entry ───────────────────────────────────────────────────────────── */

uint32_t elf_entry(const elf32_ehdr_t *ehdr)
{
    return ehdr->e_entry;
}
