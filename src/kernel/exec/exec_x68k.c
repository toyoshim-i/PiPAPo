/*
 * exec_x68k.c — Human68k binary loaders (X-format and R-format)
 *
 * Detects .x and .r format binaries and orchestrates their loading.
 * X-format includes headers and relocation; R-format is raw flat binary.
 * Loader setup (PMB initialization, register patching) is delegated to
 * human68k_loader.c.
 */

#include "exec_x68k.h"
#include "exec.h"
#include "kernel/mm/page.h"
#include "kernel/signal/signal.h"
#include "kernel/subsys/subsys.h"
#include "kernel/subsys/human68k_loader.h"
#include "kernel/endian.h"
#include "kernel/errno.h"
#include <string.h>
#if !defined(__m68k__)
#include "kernel/ecpu/ecpu_m68k.h"
#include "kernel/syscall/syscall.h"
#endif

/* PMB size: 256 bytes at the start of the process memory block.
 * Program text starts at base+0x100.  See §4.5 / §7.2. */
#define X68K_PMB_SIZE  0x100

#if !defined(__m68k__)
#define H68K_EMU_MEM_PAGES_MAX  (USER_PAGES_MAX - 1u)

typedef struct {
    m68k_state_t m68k;
    int32_t exit_code;
} h68k_emu_exec_state_t;

static inline uint16_t h68k_emu_ustack_u16(m68k_state_t *cpu, uint32_t offset)
{
    return m68k_read16(cpu, cpu->a[7] + offset);
}

static inline uint32_t h68k_emu_ustack_u32(m68k_state_t *cpu, uint32_t offset)
{
    return m68k_read32(cpu, cpu->a[7] + offset);
}

static void h68k_emu_putc(uint8_t ch)
{
    sys_write(1, (const char *)&ch, 1);
}

static uint8_t h68k_emu_getc(void)
{
    uint8_t ch = 0;
    sys_read(0, (char *)&ch, 1);
    return ch;
}

static int h68k_emu_print(m68k_state_t *cpu, uint32_t guest_addr)
{
    uint32_t i;
    for (i = 0; i < cpu->mem_size; i++) {
        uint8_t ch = m68k_read8(cpu, guest_addr + i);
        if (ch == 0)
            break;
        h68k_emu_putc(ch);
    }
    return 0;
}

static int h68k_emu_trap_handler(ecpu_state_t *state, int trap_type,
                                 uint32_t param, void *ctx)
{
    m68k_state_t *cpu = (m68k_state_t *)state;
    h68k_emu_exec_state_t *st = (h68k_emu_exec_state_t *)ctx;

    if (trap_type == ECPU_TRAP_ILLEGAL) {
        uint16_t opcode = (uint16_t)param;
        if ((opcode & 0xFF00u) == 0xFF00u) {
            uint8_t func = (uint8_t)(opcode & 0xFFu);
            if (func >= 0x80u && func <= 0xAFu)
                func = (uint8_t)(func - 0x30u);

            switch (func) {
            case 0x00: /* _EXIT */
            case 0x4C: { /* _EXIT2 */
                uint16_t code = h68k_emu_ustack_u16(cpu, 0);
                st->exit_code = (int16_t)code;
                return ECPU_TRAP_EXIT;
            }

            case 0x4A: /* _SETBLOCK */
                cpu->d[0] = 0;
                return ECPU_TRAP_HANDLED;

            case 0x01: { /* _GETCHAR (echo) */
                uint8_t ch = h68k_emu_getc();
                h68k_emu_putc(ch);
                cpu->d[0] = (uint32_t)ch;
                return ECPU_TRAP_HANDLED;
            }

            case 0x02: /* _PUTCHAR */
            case 0x04: { /* _COMOUT */
                uint8_t ch = (uint8_t)h68k_emu_ustack_u16(cpu, 0);
                h68k_emu_putc(ch);
                cpu->d[0] = (uint32_t)ch;
                return ECPU_TRAP_HANDLED;
            }

            case 0x03: { /* _COMINP (raw read) */
                uint8_t ch = h68k_emu_getc();
                cpu->d[0] = (uint32_t)ch;
                return ECPU_TRAP_HANDLED;
            }

            case 0x05: { /* _MOVE */
                uint8_t ch = (uint8_t)h68k_emu_ustack_u16(cpu, 0);
                h68k_emu_putc(ch);
                cpu->d[0] = 0;
                return ECPU_TRAP_HANDLED;
            }

            case 0x08: { /* _INPOUT */
                uint16_t code = h68k_emu_ustack_u16(cpu, 0);
                if (code == 0x00FFu) {
                    cpu->d[0] = 0;
                } else {
                    h68k_emu_putc((uint8_t)code);
                    cpu->d[0] = (uint32_t)code;
                }
                return ECPU_TRAP_HANDLED;
            }

            case 0x09: { /* _PRINT */
                uint32_t str_addr = h68k_emu_ustack_u32(cpu, 0);
                cpu->d[0] = (uint32_t)h68k_emu_print(cpu, str_addr);
                return ECPU_TRAP_HANDLED;
            }

            case 0x0C: { /* _KFLUSH */
                uint16_t mode = h68k_emu_ustack_u16(cpu, 0);
                switch (mode) {
                case 0x01: { /* like _GETCHAR */
                    uint8_t ch = h68k_emu_getc();
                    h68k_emu_putc(ch);
                    cpu->d[0] = (uint32_t)ch;
                    break;
                }
                case 0x07: { /* raw wait */
                    uint8_t ch = h68k_emu_getc();
                    cpu->d[0] = (uint32_t)ch;
                    break;
                }
                case 0x06: /* no buffered input */
                case 0x08:
                default:
                    cpu->d[0] = 0;
                    break;
                }
                return ECPU_TRAP_HANDLED;
            }

            case 0x10: { /* _CONCTRL */
                uint16_t sub = h68k_emu_ustack_u16(cpu, 0);
                switch (sub) {
                case 0x00: { /* putc */
                    uint8_t ch = (uint8_t)h68k_emu_ustack_u16(cpu, 2);
                    h68k_emu_putc(ch);
                    cpu->d[0] = (uint32_t)ch;
                    break;
                }
                case 0x01: { /* print */
                    uint32_t str_addr = h68k_emu_ustack_u32(cpu, 2);
                    cpu->d[0] = (uint32_t)h68k_emu_print(cpu, str_addr);
                    break;
                }
                case 0x0A: /* check break */
                case 0x0B: /* fn-key mode */
                default:
                    cpu->d[0] = 0;
                    break;
                }
                return ECPU_TRAP_HANDLED;
            }

            case 0x19: /* _CURDRV */
                cpu->d[0] = 0;
                return ECPU_TRAP_HANDLED;

            case 0x1A: /* _ALLCLOSE */
                cpu->d[0] = 0;
                return ECPU_TRAP_HANDLED;

            case 0x20: { /* _SUPER */
                uint32_t ssp = h68k_emu_ustack_u32(cpu, 0);
                cpu->d[0] = (ssp == 0) ? cpu->a[7] : 0;
                return ECPU_TRAP_HANDLED;
            }

            case 0x24: /* _FFLUSH */
                cpu->d[0] = 0;
                return ECPU_TRAP_HANDLED;

            case 0x30: /* _VERNUM */
                cpu->d[0] = 0x36380302u;
                return ECPU_TRAP_HANDLED;

            case 0x33: /* _BREAKCK */
                cpu->d[0] = 1;
                return ECPU_TRAP_HANDLED;

            case 0x35: /* _INTVCG */
                cpu->d[0] = 0;
                return ECPU_TRAP_HANDLED;

            case 0x51: /* _GETPDB */
                cpu->d[0] = 0;
                return ECPU_TRAP_HANDLED;

            default:
                cpu->d[0] = (uint32_t)(-(int32_t)ENOSYS);
                return ECPU_TRAP_HANDLED;
            }
        }
        st->exit_code = 132;
        return ECPU_TRAP_EXIT;
    }

    if (trap_type == ECPU_TRAP_HALT) {
        st->exit_code = 0;
        return ECPU_TRAP_EXIT;
    }

    return ECPU_TRAP_UNHANDLED;
}

static void h68k_emu_run_process(void)
{
    pcb_t *p = current;
    h68k_emu_exec_state_t *st = (h68k_emu_exec_state_t *)p->subsys_data;
    st->exit_code = 0;
    ecpu_m68k_ops.run((ecpu_state_t *)&st->m68k);
    sys_exit((long)st->exit_code);
}
#endif

/* ── Detection ─────────────────────────────────────────────────────────────── */

int x68k_detect(const uint8_t *file, uint32_t size)
{
    if (size < X68K_HEADER_SIZE)
        return 0;
    return file[0] == X68K_MAGIC_0 && file[1] == X68K_MAGIC_1;
}

/* ── Header validation ─────────────────────────────────────────────────────── */

int x68k_validate(const x68k_header_t *hdr, uint32_t file_size)
{
    uint32_t entry_offset = be32_load(&hdr->entry_offset);
    uint32_t text_size = be32_load(&hdr->text_size);
    uint32_t data_size = be32_load(&hdr->data_size);
    uint32_t bss_size = be32_load(&hdr->bss_size);
    uint32_t reloc_size = be32_load(&hdr->reloc_size);

    if (hdr->magic[0] != X68K_MAGIC_0 || hdr->magic[1] != X68K_MAGIC_1)
        return -(int)ENOEXEC;

    if (entry_offset >= text_size && text_size > 0)
        return -(int)ENOEXEC;

    uint32_t needed = X68K_HEADER_SIZE + text_size + data_size + reloc_size;
    if (needed > file_size)
        return -(int)ENOEXEC;

    uint32_t image_size = text_size + data_size + bss_size;
    if (image_size < text_size)
        return -(int)ENOEXEC;

    if (text_size == 0)
        return -(int)ENOEXEC;

    return 0;
}

/* ── Relocation processor ──────────────────────────────────────────────────── */

int x68k_apply_relocs(uint8_t *image, uint32_t image_size,
                      const uint8_t *reloc_table, uint32_t reloc_size,
                      uint32_t delta)
{
    if (reloc_size == 0 || delta == 0)
        return 0;

    uint32_t pos = 0;
    uint32_t fixup = 0;
    int first = 1;

    while (pos < reloc_size) {
        uint32_t disp;

        if (pos + 2 > reloc_size)
            break;

        uint16_t d16 = be16_load(reloc_table + pos);
        pos += 2;

        if (first) {
            fixup = d16;
            first = 0;
        } else {
            if (d16 == 0x0001) {
                if (pos + 4 > reloc_size)
                    return -(int)ENOEXEC;
                disp = be32_load(reloc_table + pos);
                pos += 4;
                fixup += disp;
            } else {
                fixup += d16;
            }
        }

        if (fixup & 1) {
            uint32_t off = fixup & ~1u;
            if (off + 2 > image_size)
                return -(int)ENOEXEC;
            uint16_t val = be16_load(image + off);
            be16_store(image + off, (uint16_t)(val + delta));
        } else {
            if (fixup + 4 > image_size)
                return -(int)ENOEXEC;
            uint32_t val = be32_load(image + fixup);
            be32_store(image + fixup, val + delta);
        }
    }

    return 0;
}

/* ── Loader ────────────────────────────────────────────────────────────────── */

/*
 * exec_x68k — Load an X-format binary and set up a process to run it.
 *
 * Memory layout (§7.2):
 *   base+0x0000  PMB (256 bytes: MMB header + process fields)
 *   base+0x0100  Text segment
 *   base+text    Data segment
 *   base+data    BSS (zeroed)
 *   ...          Heap (grows up)
 *   top          Stack (grows down)
 *
 * Human68k protocol (§7.3): allocate up to USER_PAGES_MAX pages.
 * The program calls _SETBLOCK at startup to release unneeded pages.
 *
 * Initial registers (§4.3):
 *   a0 = PMB base         a1 = memory end + 1
 *   a2 = command line      a3 = environment (−1 = none)
 *   a4 = program start (base+0x100)
 */
int exec_x68k(pcb_t *p, const uint8_t *file, uint32_t size,
              const char *path, const char *const *argv)
{
#if !defined(__m68k__)
    const x68k_header_t *hdr = (const x68k_header_t *)file;
    uint32_t entry_offset;
    uint32_t text_size;
    uint32_t data_size;
    uint32_t bss_size;
    uint32_t reloc_size;
    uint32_t base_addr;
    uint32_t image_size;
    uint32_t min_pages;
    uint32_t min_total_pages;
    uint32_t total_pages;
    uint32_t emu_mem_pages;
    uint32_t emu_mem_size;
    uint8_t *mem_base;
    uint8_t *emu_mem;
    h68k_emu_exec_state_t *st;
    uint32_t i;
    int err;

    (void)argv;

    err = x68k_validate(hdr, size);
    if (err < 0)
        return err;

    entry_offset = be32_load(&hdr->entry_offset);
    text_size = be32_load(&hdr->text_size);
    data_size = be32_load(&hdr->data_size);
    bss_size = be32_load(&hdr->bss_size);
    reloc_size = be32_load(&hdr->reloc_size);
    base_addr = be32_load(&hdr->base_addr);

    image_size = text_size + data_size + bss_size;
    min_pages = (X68K_PMB_SIZE + image_size + PAGE_SIZE - 1u) / PAGE_SIZE;
    if (min_pages > H68K_EMU_MEM_PAGES_MAX)
        return -(int)ENOMEM;

    p->stack_page = page_alloc();
    if (!p->stack_page)
        return -(int)ENOMEM;

    min_total_pages = min_pages + 1u; /* +1 page for emu state */
    total_pages = USER_PAGES_MAX;
    mem_base = NULL;
    while (total_pages >= min_total_pages) {
        mem_base = alloc_contiguous(total_pages);
        if (mem_base)
            break;
        if (total_pages == min_total_pages)
            break;
        total_pages--;
    }
    if (!mem_base) {
        page_free(p->stack_page);
        p->stack_page = NULL;
        return -(int)ENOMEM;
    }

    emu_mem_pages = total_pages - 1u;
    emu_mem_size = emu_mem_pages * PAGE_SIZE;

    for (i = 0; i < total_pages; i++)
        p->user_pages[i] = mem_base + i * PAGE_SIZE;

    emu_mem = mem_base;
    st = (h68k_emu_exec_state_t *)(mem_base + emu_mem_pages * PAGE_SIZE);
    memset(st, 0, sizeof(*st));
    ecpu_m68k_ops.init((ecpu_state_t *)&st->m68k, emu_mem, emu_mem_size);
    ecpu_m68k_ops.set_trap_handler((ecpu_state_t *)&st->m68k,
                                   h68k_emu_trap_handler, st);

    /* PMB + text/data/bss at guest addresses 0x0000.. */
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
        be32_store(emu_mem + 0x30, X68K_PMB_SIZE + image_size);
        be32_store(emu_mem + 0x34, X68K_PMB_SIZE + image_size);
        be32_store(emu_mem + 0x38, guest_end);
    }

    memcpy(emu_mem + X68K_PMB_SIZE, file + X68K_HEADER_SIZE, text_size);
    if (data_size > 0)
        memcpy(emu_mem + X68K_PMB_SIZE + text_size,
               file + X68K_HEADER_SIZE + text_size, data_size);
    if (bss_size > 0)
        memset(emu_mem + X68K_PMB_SIZE + text_size + data_size, 0, bss_size);

    if (reloc_size > 0) {
        uint32_t delta = X68K_PMB_SIZE - base_addr;
        const uint8_t *reloc_data = file + X68K_HEADER_SIZE + text_size + data_size;
        err = x68k_apply_relocs(emu_mem + X68K_PMB_SIZE, image_size,
                                reloc_data, reloc_size, delta);
        if (err < 0) {
            for (i = 0; i < total_pages; i++) {
                if (p->user_pages[i]) {
                    page_free(p->user_pages[i]);
                    p->user_pages[i] = NULL;
                }
            }
            page_free(p->stack_page);
            p->stack_page = NULL;
            return err;
        }
    }

    st->m68k.pc = X68K_PMB_SIZE + entry_offset;
    st->m68k.a[0] = 0x00000000u;
    st->m68k.a[1] = emu_mem_size;
    st->m68k.a[2] = 0x0000006Cu;
    st->m68k.a[3] = 0xFFFFFFFFu;
    st->m68k.a[4] = X68K_PMB_SIZE;
    st->m68k.a[7] = emu_mem_size;

    p->subsys = SUBSYS_PPAP;
    p->subsys_data = st;
    proc_setup_stack(p, h68k_emu_run_process, 0);

    {
        const char *bname = path;
        for (const char *s = path; *s; s++) {
            if (*s == '/')
                bname = s + 1;
        }
        size_t clen = strlen(bname);
        if (clen > 15) clen = 15;
        memcpy(p->comm, bname, clen);
        p->comm[clen] = '\0';
    }

    if (current)
        memcpy(p->cwd, current->cwd, sizeof(p->cwd));
    else
        strcpy(p->cwd, "/");

    for (i = 0; i < NSIG; i++) {
        if (p->sig_handlers[i] != SIG_IGN)
            p->sig_handlers[i] = SIG_DFL;
    }
    p->sig_pending = 0;
    p->sig_blocked = 0;

    return 0;
#else
    const x68k_header_t *hdr = (const x68k_header_t *)file;
    uint32_t entry_offset;
    uint32_t text_size;
    uint32_t data_size;
    uint32_t bss_size;
    uint32_t reloc_size;
    uint32_t base_addr;

    int err = x68k_validate(hdr, size);
    if (err < 0)
        return err;

    entry_offset = be32_load(&hdr->entry_offset);
    text_size = be32_load(&hdr->text_size);
    data_size = be32_load(&hdr->data_size);
    bss_size = be32_load(&hdr->bss_size);
    reloc_size = be32_load(&hdr->reloc_size);
    base_addr = be32_load(&hdr->base_addr);

    /* ── 1. Calculate memory requirements ──────────────────────────────── */
    uint32_t image_size = text_size + data_size + bss_size;
    uint32_t min_bytes = X68K_PMB_SIZE + image_size;
    uint32_t min_pages = (min_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    if (min_pages > USER_PAGES_MAX)
        return -(int)ENOMEM;

    /* ── 2. Allocate stack page (before data — same LIFO drain as ELF) ── */
    void *stack = page_alloc();
    if (!stack)
        return -(int)ENOMEM;
    p->stack_page = stack;

    /* ── 3. Allocate contiguous pages ──────────────────────────────────
     *
     * Human68k protocol: try USER_PAGES_MAX first, fall back to minimum.
     * The program sees the entire block as its own and calls _SETBLOCK
     * to release what it doesn't need.
     */
    uint32_t n_pages = USER_PAGES_MAX;
    uint8_t *base = NULL;
    while (n_pages >= min_pages) {
        base = alloc_contiguous(n_pages);
        if (base)
            break;
        n_pages--;
    }
    if (!base) {
        page_free(stack);
        p->stack_page = NULL;
        return -(int)ENOMEM;
    }

    uint32_t total_bytes = n_pages * PAGE_SIZE;

    for (uint32_t i = 0; i < n_pages; i++)
        p->user_pages[i] = base + i * PAGE_SIZE;

    /* ── 4. Zero PMB, copy text+data, zero BSS ────────────────────────── */
    memset(base, 0, X68K_PMB_SIZE);

    uint8_t *text_dst = base + X68K_PMB_SIZE;

    /* ── 4a. Populate PMB fields (§4.4 MMB + §4.5 PMB) ──────────────── */
    x68k_setup_pmb(base, total_bytes, image_size, path);

    const uint8_t *text_src = file + X68K_HEADER_SIZE;

    memcpy(text_dst, text_src, text_size);

    if (data_size > 0)
        memcpy(text_dst + text_size, text_src + text_size, data_size);

    if (bss_size > 0)
        memset(text_dst + text_size + data_size, 0, bss_size);

    /* ── 5. Apply relocations ──────────────────────────────────────────── */
    if (reloc_size > 0) {
        uint32_t load_addr = (uint32_t)(uintptr_t)text_dst;
        uint32_t delta = load_addr - base_addr;
        const uint8_t *reloc_data = file + X68K_HEADER_SIZE
                                  + text_size + data_size;
        err = x68k_apply_relocs(text_dst, image_size,
                                reloc_data, reloc_size, delta);
        if (err < 0) {
            for (uint32_t i = 0; i < n_pages; i++)
                page_free(base + i * PAGE_SIZE);
            page_free(stack);
            p->stack_page = NULL;
            return err;
        }
    }

    /* ── 6. Set up entry point and stack frame ─────────────────────────
     *
     * Human68k uses the allocated block itself for user stack — USP
     * starts at the top of the block and grows down.  stack_page is
     * the kernel stack (SSP) for exception handling.
     */
    uint32_t entry = (uint32_t)(uintptr_t)(text_dst + entry_offset);
    proc_setup_stack(p, (void (*)(void))(uintptr_t)entry, 0);

    /* USP = top of allocated block (user stack for Human68k) */
#if defined(__m68k__)
    p->usp = (uint32_t)(uintptr_t)(base + total_bytes);
#endif

    /* ── 7. Patch initial registers in the software frame ──────────────
     *
     * proc_setup_stack uses stack_page; we patch the register slots.
     */
    x68k_setup_registers(p->sp, (uint32_t)(uintptr_t)base,
                         (uint32_t)(uintptr_t)(base + total_bytes),
                         (uint32_t)(uintptr_t)(base + 0x6C),
                         (uint32_t)(uintptr_t)text_dst);

    /* ── 8. Tag as Human68k process and initialize subsystem state ──────── */
    p->subsys = SUBSYS_HUMAN68K;
    {
        const subsys_ops_t *ops = subsys_ops_table[SUBSYS_HUMAN68K];
        if (ops && ops->on_init)
            ops->on_init(p);
    }

    /* ── 9. Process metadata ───────────────────────────────────────────── */
    {
        const char *bname = path;
        for (const char *s = path; *s; s++) {
            if (*s == '/')
                bname = s + 1;
        }
        size_t clen = strlen(bname);
        if (clen > 15) clen = 15;
        memcpy(p->comm, bname, clen);
        p->comm[clen] = '\0';
    }

    if (current)
        memcpy(p->cwd, current->cwd, sizeof(p->cwd));
    else
        strcpy(p->cwd, "/");

    for (int i = 0; i < NSIG; i++) {
        if (p->sig_handlers[i] != (sighandler_t)1 /* SIG_IGN */)
            p->sig_handlers[i] = (sighandler_t)0;  /* SIG_DFL */
    }
    p->sig_pending = 0;
    p->sig_blocked = 0;

    (void)argv;   /* TODO: build LASCIIZ command line */
    (void)size;

    return 0;
#endif
}

/* ── R-format detection ───────────────────────────────────────────────────── */

int r68k_detect(const char *path, const uint8_t *file, uint32_t size)
{
    /* R-format has no magic — detect by ".r" or ".R" extension */
    if (size < 2)
        return 0;

    /* Must NOT be X-format (HU magic) or ELF */
    if (file[0] == X68K_MAGIC_0 && file[1] == X68K_MAGIC_1)
        return 0;
    if (file[0] == 0x7F && file[1] == 'E')
        return 0;

    /* Check extension */
    const char *dot = NULL;
    for (const char *s = path; *s; s++) {
        if (*s == '.')
            dot = s;
        else if (*s == '/')
            dot = NULL;
    }
    if (!dot)
        return 0;
    return (dot[1] == 'r' || dot[1] == 'R') && dot[2] == '\0';
}

/* ── R-format loader ──────────────────────────────────────────────────────── *
 *
 * R-format is a raw flat binary with no header.  The entire file is
 * loaded at base+0x100 (after the PMB).  Entry point is the start
 * of the loaded image.  No relocation is applied.
 *
 * Memory layout matches X-format (§7.2):
 *   base+0x0000  PMB (256 bytes)
 *   base+0x0100  Program image (entire file)
 *   ...          Stack (grows down from top of block)
 */
int exec_r68k(pcb_t *p, const uint8_t *file, uint32_t size,
              const char *path, const char *const *argv)
{
#if !defined(__m68k__)
    (void)p;
    (void)file;
    (void)size;
    (void)path;
    (void)argv;
    return -(int)ENOEXEC;
#else
    if (size == 0)
        return -(int)ENOEXEC;

    /* ── 1. Calculate memory requirements ──────────────────────────────── */
    uint32_t min_bytes = X68K_PMB_SIZE + size;
    uint32_t min_pages = (min_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    if (min_pages > USER_PAGES_MAX)
        return -(int)ENOMEM;

    /* ── 2. Allocate kernel stack page ─────────────────────────────────── */
    void *stack = page_alloc();
    if (!stack)
        return -(int)ENOMEM;
    p->stack_page = stack;

    /* ── 3. Allocate contiguous pages (same Human68k protocol) ─────────── */
    uint32_t n_pages = USER_PAGES_MAX;
    uint8_t *base = NULL;
    while (n_pages >= min_pages) {
        base = alloc_contiguous(n_pages);
        if (base)
            break;
        n_pages--;
    }
    if (!base) {
        page_free(stack);
        p->stack_page = NULL;
        return -(int)ENOMEM;
    }

    uint32_t total_bytes = n_pages * PAGE_SIZE;

    for (uint32_t i = 0; i < n_pages; i++)
        p->user_pages[i] = base + i * PAGE_SIZE;

    /* ── 4. Zero PMB, copy file image ──────────────────────────────────── */
    memset(base, 0, X68K_PMB_SIZE);
    uint8_t *image_dst = base + X68K_PMB_SIZE;
    memcpy(image_dst, file, size);

    /* Zero remaining space after image (acts as BSS + stack area) */
    if (X68K_PMB_SIZE + size < total_bytes)
        memset(image_dst + size, 0, total_bytes - X68K_PMB_SIZE - size);

    /* ── 4a. Populate PMB fields ───────────────────────────────────────── */
    x68k_setup_pmb(base, total_bytes, size, path);

    /* ── 5. Set up entry point (no relocation needed) ──────────────────── */
    uint32_t entry = (uint32_t)(uintptr_t)image_dst;
    proc_setup_stack(p, (void (*)(void))(uintptr_t)entry, 0);

#if defined(__m68k__)
    p->usp = (uint32_t)(uintptr_t)(base + total_bytes);
#endif

    /* ── 6. Patch initial registers ────────────────────────────────────── */
    x68k_setup_registers(p->sp, (uint32_t)(uintptr_t)base,
                         (uint32_t)(uintptr_t)(base + total_bytes),
                         (uint32_t)(uintptr_t)(base + 0x6C),
                         (uint32_t)(uintptr_t)image_dst);

    /* ── 7. Tag as Human68k and initialize subsystem ───────────────────── */
    p->subsys = SUBSYS_HUMAN68K;
    {
        const subsys_ops_t *ops = subsys_ops_table[SUBSYS_HUMAN68K];
        if (ops && ops->on_init)
            ops->on_init(p);
    }

    /* ── 8. Process metadata ───────────────────────────────────────────── */
    {
        const char *bname = path;
        for (const char *s = path; *s; s++) {
            if (*s == '/')
                bname = s + 1;
        }
        size_t clen = strlen(bname);
        if (clen > 15) clen = 15;
        memcpy(p->comm, bname, clen);
        p->comm[clen] = '\0';
    }

    if (current)
        memcpy(p->cwd, current->cwd, sizeof(p->cwd));
    else
        strcpy(p->cwd, "/");

    for (int i = 0; i < NSIG; i++) {
        if (p->sig_handlers[i] != (sighandler_t)1)
            p->sig_handlers[i] = (sighandler_t)0;
    }
    p->sig_pending = 0;
    p->sig_blocked = 0;

    (void)argv;
    return 0;
#endif
}
