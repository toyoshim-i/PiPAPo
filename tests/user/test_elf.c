/*
 * test_elf.c — On-target tests for src/kernel/exec/elf.c
 *
 * Builds synthetic ELF headers in user memory and exercises the parser
 * directly in the target environment. This keeps ELF parser coverage in the
 * normal user test suite instead of relying on a host-only build.
 */

#include "utest.h"
#include "kernel/core/exec/elf.h"
#include "common/errno.h"
#include <stdint.h>
#include <stddef.h>

/* Build the parser into this user test directly. */
#include "kernel/core/exec/elf.c"

static void mem_zero(void *dst, size_t len)
{
    uint8_t *p = (uint8_t *)dst;
    for (size_t i = 0; i < len; i++)
        p[i] = 0;
}

static void mem_copy(void *dst, const void *src, size_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < len; i++)
        d[i] = s[i];
}

static void init_valid_ehdr(elf32_ehdr_t *h)
{
    mem_zero(h, sizeof(*h));

    h->e_ident[EI_MAG0] = ELFMAG0;
    h->e_ident[EI_MAG1] = ELFMAG1;
    h->e_ident[EI_MAG2] = ELFMAG2;
    h->e_ident[EI_MAG3] = ELFMAG3;
    h->e_ident[EI_CLASS] = ELFCLASS32;
#if defined(__m68k__)
    h->e_ident[EI_DATA] = ELFDATA2MSB;
    h->e_machine = EM_68K;
#elif defined(__xtensa__)
    h->e_ident[EI_DATA] = ELFDATA2LSB;
    h->e_machine = EM_XTENSA;
#elif defined(__riscv)
    h->e_ident[EI_DATA] = ELFDATA2LSB;
    h->e_machine = EM_RISCV;
#else
    h->e_ident[EI_DATA] = ELFDATA2LSB;
    h->e_machine = EM_ARM;
    h->e_flags = EF_ARM_EABI_VER5 | EF_ARM_ABI_FLOAT_SOFT;
#endif
    h->e_type = ET_EXEC;
    h->e_version = 1;
    h->e_entry = 1;
    h->e_phoff = sizeof(elf32_ehdr_t);
    h->e_phentsize = sizeof(elf32_phdr_t);
    h->e_phnum = 1;
    h->e_ehsize = sizeof(elf32_ehdr_t);
}

static void init_load_phdr(elf32_phdr_t *ph)
{
    mem_zero(ph, sizeof(*ph));
    ph->p_type = PT_LOAD;
    ph->p_offset = 0x0;
    ph->p_vaddr = 0;
    ph->p_paddr = 0;
    ph->p_filesz = 0x44;
    ph->p_memsz = 0x44;
    ph->p_flags = PF_R | PF_X;
    ph->p_align = 0x1000;
}

static void test_validate_valid_elf(void)
{
    elf32_ehdr_t h;
    init_valid_ehdr(&h);
    UT_ASSERT_EQ(elf_validate(&h), 0);
}

static void test_validate_invalid_magic(void)
{
    elf32_ehdr_t h;
    init_valid_ehdr(&h);
    h.e_ident[EI_MAG0] = 0;
    UT_ASSERT_EQ(elf_validate(&h), -(int)ENOEXEC);
}

static void test_validate_wrong_class(void)
{
    elf32_ehdr_t h;
    init_valid_ehdr(&h);
    h.e_ident[EI_CLASS] = 2;
    UT_ASSERT_EQ(elf_validate(&h), -(int)ENOEXEC);
}

static void test_validate_wrong_endian(void)
{
    elf32_ehdr_t h;
    init_valid_ehdr(&h);
#if defined(__m68k__)
    h.e_ident[EI_DATA] = ELFDATA2LSB;
#else
    h.e_ident[EI_DATA] = ELFDATA2MSB;
#endif
    UT_ASSERT_EQ(elf_validate(&h), -(int)ENOEXEC);
}

static void test_validate_wrong_machine(void)
{
    elf32_ehdr_t h;
    init_valid_ehdr(&h);
    /* Any architecture that isn't the build target. */
#if defined(__m68k__)
    h.e_machine = EM_ARM;
#else
    h.e_machine = EM_68K;
#endif
    UT_ASSERT_EQ(elf_validate(&h), -(int)ENOEXEC);
}

static void test_validate_wrong_type(void)
{
    elf32_ehdr_t h;
    init_valid_ehdr(&h);
    h.e_type = 1;
    UT_ASSERT_EQ(elf_validate(&h), -(int)ENOEXEC);
}

static void test_validate_et_dyn_accepted(void)
{
    elf32_ehdr_t h;
    init_valid_ehdr(&h);
    h.e_type = ET_DYN;
    UT_ASSERT_EQ(elf_validate(&h), 0);
}

static void test_validate_no_program_headers(void)
{
    elf32_ehdr_t h;
    init_valid_ehdr(&h);
    h.e_phnum = 0;
    UT_ASSERT_EQ(elf_validate(&h), -(int)ENOEXEC);
}

static void test_validate_no_phoff(void)
{
    elf32_ehdr_t h;
    init_valid_ehdr(&h);
    h.e_phoff = 0;
    UT_ASSERT_EQ(elf_validate(&h), -(int)ENOEXEC);
}

#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
static void test_validate_wrong_eabi_version(void)
{
    elf32_ehdr_t h;
    init_valid_ehdr(&h);
    h.e_flags = 0x04000000u;
    UT_ASSERT_EQ(elf_validate(&h), -(int)ENOEXEC);
}
#endif

static void test_load_one_segment(void)
{
    uint8_t buf[sizeof(elf32_ehdr_t) + sizeof(elf32_phdr_t)];
    elf32_ehdr_t h;
    elf32_phdr_t ph;
    init_valid_ehdr(&h);
    init_load_phdr(&ph);
    mem_copy(buf, &h, sizeof(h));
    mem_copy(buf + sizeof(h), &ph, sizeof(ph));

    elf32_phdr_t out[4];
    int n = elf_load_segments((const elf32_ehdr_t *)buf, buf, out, 4, sizeof(buf));
    UT_ASSERT_EQ(n, 1);
    UT_ASSERT_EQ((int)out[0].p_type, PT_LOAD);
    UT_ASSERT_EQ((int)out[0].p_vaddr, 0);
    UT_ASSERT_EQ((int)out[0].p_filesz, 0x44);
    UT_ASSERT_EQ((int)out[0].p_flags, PF_R | PF_X);
}

static void test_load_skips_non_load(void)
{
    uint8_t buf[sizeof(elf32_ehdr_t) + 2 * sizeof(elf32_phdr_t)];
    elf32_ehdr_t h;
    init_valid_ehdr(&h);
    h.e_phnum = 2;

    elf32_phdr_t ph_note;
    mem_zero(&ph_note, sizeof(ph_note));
    ph_note.p_type = 4;

    elf32_phdr_t ph_load;
    init_load_phdr(&ph_load);

    mem_copy(buf, &h, sizeof(h));
    mem_copy(buf + sizeof(h), &ph_note, sizeof(ph_note));
    mem_copy(buf + sizeof(h) + sizeof(ph_note), &ph_load, sizeof(ph_load));

    elf32_phdr_t out[4];
    int n = elf_load_segments((const elf32_ehdr_t *)buf, buf, out, 4, sizeof(buf));
    UT_ASSERT_EQ(n, 1);
    UT_ASSERT_EQ((int)out[0].p_vaddr, 0);
}

static void test_load_no_load_segments(void)
{
    uint8_t buf[sizeof(elf32_ehdr_t) + sizeof(elf32_phdr_t)];
    elf32_ehdr_t h;
    init_valid_ehdr(&h);

    elf32_phdr_t ph_note;
    mem_zero(&ph_note, sizeof(ph_note));
    ph_note.p_type = 4;

    mem_copy(buf, &h, sizeof(h));
    mem_copy(buf + sizeof(h), &ph_note, sizeof(ph_note));

    elf32_phdr_t out[4];
    int n = elf_load_segments((const elf32_ehdr_t *)buf, buf, out, 4, sizeof(buf));
    UT_ASSERT_EQ(n, 0);
}

static void test_load_overflow_count(void)
{
    uint8_t buf[sizeof(elf32_ehdr_t) + 3 * sizeof(elf32_phdr_t)];
    elf32_ehdr_t h;
    init_valid_ehdr(&h);
    h.e_phnum = 3;

    elf32_phdr_t ph;
    init_load_phdr(&ph);

    mem_copy(buf, &h, sizeof(h));
    for (int i = 0; i < 3; i++)
        mem_copy(buf + sizeof(h) + i * sizeof(ph), &ph, sizeof(ph));

    elf32_phdr_t out[2];
    mem_zero(out, sizeof(out));
    int n = elf_load_segments((const elf32_ehdr_t *)buf, buf, out, 2, sizeof(buf));
    UT_ASSERT_EQ(n, 3);
    UT_ASSERT_EQ((int)out[0].p_type, PT_LOAD);
    UT_ASSERT_EQ((int)out[1].p_type, PT_LOAD);
}

static void test_entry_returns_address(void)
{
    elf32_ehdr_t h;
    init_valid_ehdr(&h);
    h.e_entry = 1;
    UT_ASSERT_EQ((int)elf_entry(&h), 1);
}

static void test_entry_preserves_thumb_bit(void)
{
    elf32_ehdr_t h;
    init_valid_ehdr(&h);
    h.e_entry = 0x10001001u;
    UT_ASSERT_EQ((int)elf_entry(&h), (int)0x10001001u);
}

static const char shstrtab_data[] = "\0.text\0.got\0.shstrtab\0";

#define TEST_SHSTRTAB_OFF  0x118
#define TEST_SHDR_OFF      0x200

static void build_got_test_elf(uint8_t *buf, size_t bufsz)
{
    mem_zero(buf, bufsz);

    elf32_ehdr_t *ehdr = (elf32_ehdr_t *)buf;
    elf32_ehdr_t h;
    init_valid_ehdr(&h);
    h.e_phnum = 2;
    h.e_shoff = TEST_SHDR_OFF;
    h.e_shnum = 5;
    h.e_shentsize = sizeof(elf32_shdr_t);
    h.e_shstrndx = 3;
    mem_copy(ehdr, &h, sizeof(h));

    elf32_phdr_t text_ph;
    init_load_phdr(&text_ph);
    text_ph.p_offset = 0x100;
    text_ph.p_vaddr = 0;
    text_ph.p_filesz = 16;
    text_ph.p_memsz = 16;
    text_ph.p_flags = PF_R | PF_X;
    mem_copy(buf + sizeof(elf32_ehdr_t), &text_ph, sizeof(text_ph));

    elf32_phdr_t data_ph;
    mem_zero(&data_ph, sizeof(data_ph));
    data_ph.p_type = PT_LOAD;
    data_ph.p_offset = 0x110;
    data_ph.p_vaddr = 0x0010;
    data_ph.p_filesz = 8;
    data_ph.p_memsz = 8;
    data_ph.p_flags = PF_R | PF_W;
    mem_copy(buf + sizeof(elf32_ehdr_t) + sizeof(elf32_phdr_t),
             &data_ph, sizeof(data_ph));

    mem_copy(buf + TEST_SHSTRTAB_OFF, shstrtab_data, sizeof(shstrtab_data));

    elf32_shdr_t *shdrs = (elf32_shdr_t *)(buf + TEST_SHDR_OFF);
    mem_zero(&shdrs[0], sizeof(elf32_shdr_t));

    mem_zero(&shdrs[1], sizeof(elf32_shdr_t));
    shdrs[1].sh_name = 1;
    shdrs[1].sh_type = 1;
    shdrs[1].sh_flags = 6;
    shdrs[1].sh_addr = 0;
    shdrs[1].sh_offset = 0x100;
    shdrs[1].sh_size = 16;

    mem_zero(&shdrs[2], sizeof(elf32_shdr_t));
    shdrs[2].sh_name = 7;
    shdrs[2].sh_type = 1;
    shdrs[2].sh_flags = 3;
    shdrs[2].sh_addr = 0x0010;
    shdrs[2].sh_offset = 0x110;
    shdrs[2].sh_size = 8;
    shdrs[2].sh_addralign = 4;
    shdrs[2].sh_entsize = 4;

    mem_zero(&shdrs[3], sizeof(elf32_shdr_t));
    shdrs[3].sh_name = 12;
    shdrs[3].sh_type = 3;
    shdrs[3].sh_offset = TEST_SHSTRTAB_OFF;
    shdrs[3].sh_size = sizeof(shstrtab_data);

    mem_zero(&shdrs[4], sizeof(elf32_shdr_t));
}

static void test_find_got_present(void)
{
    uint8_t buf[0x400];
    build_got_test_elf(buf, sizeof(buf));

    elf_got_info_t got;
    int rc = elf_find_got((const elf32_ehdr_t *)buf, buf, &got, sizeof(buf));
    UT_ASSERT_EQ(rc, 0);
    UT_ASSERT_EQ((int)got.offset, 0x110);
    UT_ASSERT_EQ((int)got.addr, 0x0010);
    UT_ASSERT_EQ((int)got.size, 8);
}

static void test_find_got_absent(void)
{
    uint8_t buf[0x400];
    build_got_test_elf(buf, sizeof(buf));
    buf[TEST_SHSTRTAB_OFF + 7] = '.';
    buf[TEST_SHSTRTAB_OFF + 8] = 'x';
    buf[TEST_SHSTRTAB_OFF + 9] = 'x';
    buf[TEST_SHSTRTAB_OFF + 10] = 'x';

    elf_got_info_t got;
    int rc = elf_find_got((const elf32_ehdr_t *)buf, buf, &got, sizeof(buf));
    UT_ASSERT_EQ(rc, 1);
}

static void test_find_got_no_sections(void)
{
    elf32_ehdr_t h;
    init_valid_ehdr(&h);
    h.e_shoff = 0;
    h.e_shnum = 0;

    elf_got_info_t got;
    int rc = elf_find_got(&h, (const uint8_t *)&h, &got, sizeof(h));
    UT_ASSERT_EQ(rc, 1);
}

int main(void)
{
    UT_PRINT("  elf: validate\n");
    test_validate_valid_elf();
    test_validate_invalid_magic();
    test_validate_wrong_class();
    test_validate_wrong_endian();
    test_validate_wrong_machine();
    test_validate_wrong_type();
    test_validate_et_dyn_accepted();
    test_validate_no_program_headers();
    test_validate_no_phoff();
#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
    test_validate_wrong_eabi_version();
#endif

    UT_PRINT("  elf: segments\n");
    test_load_one_segment();
    test_load_skips_non_load();
    test_load_no_load_segments();
    test_load_overflow_count();

    UT_PRINT("  elf: entry\n");
    test_entry_returns_address();
    test_entry_preserves_thumb_bit();

    UT_PRINT("  elf: got\n");
    test_find_got_present();
    test_find_got_absent();
    test_find_got_no_sections();

    UT_SUMMARY("test_elf");
}
