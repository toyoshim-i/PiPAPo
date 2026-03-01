/*
 * test_elf.c — Unit tests for src/kernel/exec/elf.c
 *
 * Tests the ELF32 parser: header validation, PT_LOAD segment extraction,
 * and entry point retrieval.  Uses programmatically constructed ELF
 * headers — no real binary files needed.
 *
 * The parser is pure C with no hardware dependencies, so it compiles and
 * runs cleanly on the host.
 */

#include "test_framework.h"
#include "kernel/exec/elf.h"
#include "kernel/errno.h"
#include <string.h>

/* ── Helper: construct a valid ARM Thumb ELF32 header ────────────────────── */

static elf32_ehdr_t make_valid_ehdr(void)
{
    elf32_ehdr_t h;
    memset(&h, 0, sizeof(h));

    /* Magic */
    h.e_ident[EI_MAG0] = ELFMAG0;
    h.e_ident[EI_MAG1] = ELFMAG1;
    h.e_ident[EI_MAG2] = ELFMAG2;
    h.e_ident[EI_MAG3] = ELFMAG3;

    /* Class + data encoding */
    h.e_ident[EI_CLASS] = ELFCLASS32;
    h.e_ident[EI_DATA]  = ELFDATA2LSB;

    /* Type + machine */
    h.e_type    = ET_EXEC;
    h.e_machine = EM_ARM;
    h.e_version = 1;

    /* Entry point with Thumb bit set */
    h.e_entry = 0x00000001;

    /* Program headers: 1 entry right after the ELF header */
    h.e_phoff     = sizeof(elf32_ehdr_t);
    h.e_phentsize = sizeof(elf32_phdr_t);
    h.e_phnum     = 1;

    /* ELF header size */
    h.e_ehsize = sizeof(elf32_ehdr_t);

    /* ARM EABI version 5 + soft-float */
    h.e_flags = EF_ARM_EABI_VER5 | EF_ARM_ABI_FLOAT_SOFT;

    return h;
}

static elf32_phdr_t make_load_phdr(void)
{
    elf32_phdr_t ph;
    memset(&ph, 0, sizeof(ph));
    ph.p_type   = PT_LOAD;
    ph.p_offset = 0x1000;
    ph.p_vaddr  = 0x00000000;
    ph.p_paddr  = 0x00000000;
    ph.p_filesz = 0x44;
    ph.p_memsz  = 0x44;
    ph.p_flags  = PF_R | PF_X;
    ph.p_align  = 0x1000;
    return ph;
}

/* ── elf_validate tests ──────────────────────────────────────────────────── */

static void test_validate_valid_elf(void)
{
    elf32_ehdr_t h = make_valid_ehdr();
    ASSERT_EQ(elf_validate(&h), 0);
}

static void test_validate_invalid_magic(void)
{
    elf32_ehdr_t h = make_valid_ehdr();
    h.e_ident[EI_MAG0] = 0x00;     /* corrupt magic byte 0 */
    ASSERT_EQ(elf_validate(&h), -(int)ENOEXEC);
}

static void test_validate_wrong_class(void)
{
    elf32_ehdr_t h = make_valid_ehdr();
    h.e_ident[EI_CLASS] = 2;       /* ELFCLASS64 */
    ASSERT_EQ(elf_validate(&h), -(int)ENOEXEC);
}

static void test_validate_wrong_endian(void)
{
    elf32_ehdr_t h = make_valid_ehdr();
    h.e_ident[EI_DATA] = 2;        /* ELFDATA2MSB (big-endian) */
    ASSERT_EQ(elf_validate(&h), -(int)ENOEXEC);
}

static void test_validate_wrong_machine(void)
{
    elf32_ehdr_t h = make_valid_ehdr();
    h.e_machine = 3;               /* EM_386 (x86) */
    ASSERT_EQ(elf_validate(&h), -(int)ENOEXEC);
}

static void test_validate_wrong_type(void)
{
    elf32_ehdr_t h = make_valid_ehdr();
    h.e_type = 1;                   /* ET_REL (relocatable) */
    ASSERT_EQ(elf_validate(&h), -(int)ENOEXEC);
}

static void test_validate_et_dyn_accepted(void)
{
    elf32_ehdr_t h = make_valid_ehdr();
    h.e_type = ET_DYN;
    ASSERT_EQ(elf_validate(&h), 0);
}

static void test_validate_no_program_headers(void)
{
    elf32_ehdr_t h = make_valid_ehdr();
    h.e_phnum = 0;
    ASSERT_EQ(elf_validate(&h), -(int)ENOEXEC);
}

static void test_validate_no_phoff(void)
{
    elf32_ehdr_t h = make_valid_ehdr();
    h.e_phoff = 0;
    ASSERT_EQ(elf_validate(&h), -(int)ENOEXEC);
}

static void test_validate_wrong_eabi_version(void)
{
    elf32_ehdr_t h = make_valid_ehdr();
    h.e_flags = 0x04000000u;       /* EABI version 4 */
    ASSERT_EQ(elf_validate(&h), -(int)ENOEXEC);
}

/* ── elf_load_segments tests ─────────────────────────────────────────────── */

/*
 * Build a fake "file" in a byte buffer: ELF header followed by program
 * headers.  The file_base pointer passed to elf_load_segments must point
 * to byte 0 of the ELF file.
 */

/* File layout: [ehdr][phdr0] — 1 PT_LOAD segment */
static void test_load_one_segment(void)
{
    /* Pack ehdr + 1 phdr into a contiguous buffer */
    uint8_t buf[sizeof(elf32_ehdr_t) + sizeof(elf32_phdr_t)];
    elf32_ehdr_t h = make_valid_ehdr();
    elf32_phdr_t ph = make_load_phdr();
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), &ph, sizeof(ph));

    elf32_phdr_t out[4];
    int n = elf_load_segments((const elf32_ehdr_t *)buf, buf, out, 4);
    ASSERT_EQ(n, 1);
    ASSERT_EQ((int)out[0].p_type, PT_LOAD);
    ASSERT_EQ((int)out[0].p_vaddr, 0x00000000);
    ASSERT_EQ((int)out[0].p_filesz, 0x44);
    ASSERT_EQ((int)out[0].p_flags, PF_R | PF_X);
}

/* File layout: [ehdr][PT_NOTE phdr][PT_LOAD phdr] — only PT_LOAD extracted */
static void test_load_skips_non_load(void)
{
    uint8_t buf[sizeof(elf32_ehdr_t) + 2 * sizeof(elf32_phdr_t)];
    elf32_ehdr_t h = make_valid_ehdr();
    h.e_phnum = 2;

    /* First phdr: PT_NOTE (type = 4) */
    elf32_phdr_t ph_note;
    memset(&ph_note, 0, sizeof(ph_note));
    ph_note.p_type = 4;    /* PT_NOTE */

    /* Second phdr: PT_LOAD */
    elf32_phdr_t ph_load = make_load_phdr();

    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), &ph_note, sizeof(ph_note));
    memcpy(buf + sizeof(h) + sizeof(ph_note), &ph_load, sizeof(ph_load));

    elf32_phdr_t out[4];
    int n = elf_load_segments((const elf32_ehdr_t *)buf, buf, out, 4);
    ASSERT_EQ(n, 1);
    ASSERT_EQ((int)out[0].p_vaddr, 0x00000000);
}

/* No PT_LOAD segments at all → returns 0 */
static void test_load_no_load_segments(void)
{
    uint8_t buf[sizeof(elf32_ehdr_t) + sizeof(elf32_phdr_t)];
    elf32_ehdr_t h = make_valid_ehdr();

    elf32_phdr_t ph_note;
    memset(&ph_note, 0, sizeof(ph_note));
    ph_note.p_type = 4;    /* PT_NOTE */

    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), &ph_note, sizeof(ph_note));

    elf32_phdr_t out[4];
    int n = elf_load_segments((const elf32_ehdr_t *)buf, buf, out, 4);
    ASSERT_EQ(n, 0);
}

/* More PT_LOAD segments than output buffer → count still correct */
static void test_load_overflow_count(void)
{
    uint8_t buf[sizeof(elf32_ehdr_t) + 3 * sizeof(elf32_phdr_t)];
    elf32_ehdr_t h = make_valid_ehdr();
    h.e_phnum = 3;

    elf32_phdr_t ph = make_load_phdr();

    memcpy(buf, &h, sizeof(h));
    for (int i = 0; i < 3; i++)
        memcpy(buf + sizeof(h) + i * sizeof(ph), &ph, sizeof(ph));

    /* Output buffer holds only 2 */
    elf32_phdr_t out[2];
    memset(out, 0, sizeof(out));
    int n = elf_load_segments((const elf32_ehdr_t *)buf, buf, out, 2);
    ASSERT_EQ(n, 3);   /* total count, even though only 2 were copied */
    ASSERT_EQ((int)out[0].p_type, PT_LOAD);
    ASSERT_EQ((int)out[1].p_type, PT_LOAD);
}

/* ── elf_entry tests ─────────────────────────────────────────────────────── */

static void test_entry_returns_address(void)
{
    elf32_ehdr_t h = make_valid_ehdr();
    h.e_entry = 0x00000001;     /* Thumb bit set */
    ASSERT_EQ((int)elf_entry(&h), 0x00000001);
}

static void test_entry_preserves_thumb_bit(void)
{
    elf32_ehdr_t h = make_valid_ehdr();
    h.e_entry = 0x10001001;     /* typical XIP flash address with Thumb bit */
    ASSERT_EQ((int)elf_entry(&h), (int)0x10001001u);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== test_elf ===\n");

    TEST_GROUP("elf_validate");
    RUN_TEST(test_validate_valid_elf);
    RUN_TEST(test_validate_invalid_magic);
    RUN_TEST(test_validate_wrong_class);
    RUN_TEST(test_validate_wrong_endian);
    RUN_TEST(test_validate_wrong_machine);
    RUN_TEST(test_validate_wrong_type);
    RUN_TEST(test_validate_et_dyn_accepted);
    RUN_TEST(test_validate_no_program_headers);
    RUN_TEST(test_validate_no_phoff);
    RUN_TEST(test_validate_wrong_eabi_version);

    TEST_GROUP("elf_load_segments");
    RUN_TEST(test_load_one_segment);
    RUN_TEST(test_load_skips_non_load);
    RUN_TEST(test_load_no_load_segments);
    RUN_TEST(test_load_overflow_count);

    TEST_GROUP("elf_entry");
    RUN_TEST(test_entry_returns_address);
    RUN_TEST(test_entry_preserves_thumb_bit);

    TEST_SUMMARY();
}
