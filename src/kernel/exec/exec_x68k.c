/*
 * exec_x68k.c — Human68k X-format binary loader
 *
 * Phase 1 Steps 1–5: detection, validation, relocation, memory allocation,
 * segment loading, PMB/register setup.
 */

#include "exec_x68k.h"
#include "exec.h"
#include "kernel/mm/page.h"
#include "kernel/signal/signal.h"
#include "kernel/errno.h"
#include <string.h>

/* PMB size: 256 bytes at the start of the process memory block.
 * Program text starts at base+0x100.  See §4.5 / §7.2. */
#define X68K_PMB_SIZE  0x100

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
    if (hdr->magic[0] != X68K_MAGIC_0 || hdr->magic[1] != X68K_MAGIC_1)
        return -(int)ENOEXEC;

    if (hdr->entry_offset >= hdr->text_size && hdr->text_size > 0)
        return -(int)ENOEXEC;

    uint32_t needed = X68K_HEADER_SIZE + hdr->text_size + hdr->data_size
                    + hdr->reloc_size;
    if (needed > file_size)
        return -(int)ENOEXEC;

    uint32_t image_size = hdr->text_size + hdr->data_size + hdr->bss_size;
    if (image_size < hdr->text_size)
        return -(int)ENOEXEC;

    if (hdr->text_size == 0)
        return -(int)ENOEXEC;

    return 0;
}

/* ── Relocation processor ──────────────────────────────────────────────────── */

static uint16_t read_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void write_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

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

        uint16_t d16 = read_be16(reloc_table + pos);
        pos += 2;

        if (first) {
            fixup = d16;
            first = 0;
        } else {
            if (d16 == 0x0001) {
                if (pos + 4 > reloc_size)
                    return -(int)ENOEXEC;
                disp = read_be32(reloc_table + pos);
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
            uint16_t val = read_be16(image + off);
            write_be16(image + off, (uint16_t)(val + delta));
        } else {
            if (fixup + 4 > image_size)
                return -(int)ENOEXEC;
            uint32_t val = read_be32(image + fixup);
            write_be32(image + fixup, val + delta);
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
    const x68k_header_t *hdr = (const x68k_header_t *)file;

    int err = x68k_validate(hdr, size);
    if (err < 0)
        return err;

    /* ── 1. Calculate memory requirements ──────────────────────────────── */
    uint32_t image_size = hdr->text_size + hdr->data_size + hdr->bss_size;
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
    {
        uint32_t base_u = (uint32_t)(uintptr_t)base;
        uint32_t end_u  = base_u + total_bytes;
        uint32_t text_u = (uint32_t)(uintptr_t)text_dst;
        uint32_t bss_u  = text_u + hdr->text_size + hdr->data_size;
        uint32_t heap_u = bss_u + hdr->bss_size;

        /* MMB header (0x00–0x0F) */
        write_be32(base + 0x00, 0);          /* prev = 0 (first) */
        write_be32(base + 0x04, base_u);     /* owner = self */
        write_be32(base + 0x08, end_u);      /* block end + 1 */
        write_be32(base + 0x0C, 0);          /* next = 0 (last) */

        /* PMB fields (0x10–0xFF) */
        write_be32(base + 0x10, 0xFFFFFFFF); /* env = -1 (none) */
        /* 0x14: exit handler — filled when F-line bridge is ready */
        write_be32(base + 0x20, base_u + 0x6C); /* cmdline (empty LASCIIZ) */
        /* 0x24: file handle bitmap — stdin/stdout/stderr open */
        base[0x24] = 0x07;  /* bits 0,1,2 set = handles 0,1,2 */
        /* 0x30–0x38: segment addresses */
        write_be32(base + 0x30, bss_u);      /* BSS start */
        write_be32(base + 0x34, heap_u);     /* heap start */
        write_be32(base + 0x38, end_u);      /* initial stack (top) */

        /* 0x82: execution file path (up to 65 chars + NUL) */
        {
            const char *bname = path;
            const char *last_slash = path;
            for (const char *s = path; *s; s++) {
                if (*s == '/')
                    last_slash = s;
            }
            /* Copy directory portion to 0x82 (max 65 bytes) */
            size_t dir_len = (size_t)(last_slash - path);
            if (dir_len > 65) dir_len = 65;
            if (dir_len > 0)
                memcpy(base + 0x82, path, dir_len);
            base[0x82 + dir_len] = '\0';

            /* 0xC4: execution file name (max 23 chars + NUL) */
            bname = last_slash + 1;
            size_t name_len = strlen(bname);
            if (name_len > 23) name_len = 23;
            memcpy(base + 0xC4, bname, name_len);
            base[0xC4 + name_len] = '\0';
        }
    }
    const uint8_t *text_src = file + X68K_HEADER_SIZE;

    memcpy(text_dst, text_src, hdr->text_size);

    if (hdr->data_size > 0)
        memcpy(text_dst + hdr->text_size,
               text_src + hdr->text_size, hdr->data_size);

    if (hdr->bss_size > 0)
        memset(text_dst + hdr->text_size + hdr->data_size,
               0, hdr->bss_size);

    /* ── 5. Apply relocations ──────────────────────────────────────────── */
    if (hdr->reloc_size > 0) {
        uint32_t load_addr = (uint32_t)(uintptr_t)text_dst;
        uint32_t delta = load_addr - hdr->base_addr;
        const uint8_t *reloc_data = file + X68K_HEADER_SIZE
                                  + hdr->text_size + hdr->data_size;
        err = x68k_apply_relocs(text_dst, image_size,
                                reloc_data, hdr->reloc_size, delta);
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
     * Human68k uses the allocated block itself for stack — stack grows
     * down from the top of the block.  The stack_page is used by the
     * kernel for the initial exception frame (same as ELF loader).
     */
    uint32_t entry = (uint32_t)(uintptr_t)(text_dst + hdr->entry_offset);
    proc_setup_stack(p, (void (*)(void))(uintptr_t)entry, 0);

    /* ── 7. Patch initial registers in the software frame ──────────────
     *
     * proc_setup_stack uses stack_page; we patch the register slots.
     * Software frame (low → high from p->sp):
     *   [0..7]  d0–d7    [8..14] a0–a6    then SR(2)+PC(4)
     */
    {
        uint32_t *sw = (uint32_t *)(uintptr_t)p->sp;
        sw[8]  = (uint32_t)(uintptr_t)base;               /* a0 = PMB */
        sw[9]  = (uint32_t)(uintptr_t)(base + total_bytes);/* a1 = end+1 */
        sw[10] = (uint32_t)(uintptr_t)(base + 0x6C);        /* a2 = cmdline (empty LASCIIZ at PMB+0x6C) */
        sw[11] = 0xFFFFFFFF;                               /* a3 = env (-1) */
        sw[12] = (uint32_t)(uintptr_t)text_dst;            /* a4 = base+0x100 */
    }

    /* ── 8. Tag as Human68k process ────────────────────────────────────── */
    p->subsys = SUBSYS_HUMAN68K;

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
}
