/*
 * exec_x68k.c — Human68k X-format binary loader
 *
 * Phase 1 Steps 1–3: detection, header validation, relocation processor.
 * Subsequent steps add memory allocation, PMB setup, and F-line bridging.
 */

#include "exec_x68k.h"
#include "kernel/errno.h"

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
    /* Magic already checked by x68k_detect(), but double-check */
    if (hdr->magic[0] != X68K_MAGIC_0 || hdr->magic[1] != X68K_MAGIC_1)
        return -(int)ENOEXEC;

    /* Entry point must be within the text segment */
    if (hdr->entry_offset >= hdr->text_size && hdr->text_size > 0)
        return -(int)ENOEXEC;

    /* File must contain at least header + text + data + relocation table */
    uint32_t needed = X68K_HEADER_SIZE + hdr->text_size + hdr->data_size
                    + hdr->reloc_size;
    if (needed > file_size)
        return -(int)ENOEXEC;

    /* Overflow check: text + data + bss must not wrap */
    uint32_t image_size = hdr->text_size + hdr->data_size + hdr->bss_size;
    if (image_size < hdr->text_size)   /* overflow */
        return -(int)ENOEXEC;

    /* Must have at least a text segment */
    if (hdr->text_size == 0)
        return -(int)ENOEXEC;

    return 0;
}

/* ── Relocation processor ──────────────────────────────────────────────────
 *
 * X-format relocation table format:
 *   - First entry: 2-byte absolute displacement from image start
 *   - Subsequent entries: 2-byte relative displacement from previous fixup
 *   - If displacement == 0x0001, next 4 bytes are an extended displacement
 *   - Odd displacement → word (16-bit) relocation
 *   - Even displacement → long (32-bit) relocation
 *   - Table ends when reloc_size bytes are consumed
 *
 * Each fixup adds `delta` (= load_addr - base_addr) to the value at the
 * fixup location.
 */

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
        return 0;   /* nothing to do */

    uint32_t pos = 0;       /* position in reloc table */
    uint32_t fixup = 0;     /* current fixup offset in image */
    int first = 1;

    while (pos < reloc_size) {
        uint32_t disp;

        if (pos + 2 > reloc_size)
            break;

        uint16_t d16 = read_be16(reloc_table + pos);
        pos += 2;

        if (first) {
            /* First entry: absolute displacement from image start */
            fixup = d16;
            first = 0;
        } else {
            if (d16 == 0x0001) {
                /* Extended displacement: next 4 bytes */
                if (pos + 4 > reloc_size)
                    return -(int)ENOEXEC;
                disp = read_be32(reloc_table + pos);
                pos += 4;
                fixup += disp;
            } else {
                fixup += d16;
            }
        }

        /* Apply fixup */
        if (fixup & 1) {
            /* Odd offset → word (16-bit) relocation */
            uint32_t off = fixup & ~1u;
            if (off + 2 > image_size)
                return -(int)ENOEXEC;
            uint16_t val = read_be16(image + off);
            write_be16(image + off, (uint16_t)(val + delta));
        } else {
            /* Even offset → long (32-bit) relocation */
            if (fixup + 4 > image_size)
                return -(int)ENOEXEC;
            uint32_t val = read_be32(image + fixup);
            write_be32(image + fixup, val + delta);
        }
    }

    return 0;
}

/* ── Loader stub ───────────────────────────────────────────────────────────── */

int exec_x68k(pcb_t *p, const uint8_t *file, uint32_t size,
              const char *path, const char *const *argv)
{
    const x68k_header_t *hdr = (const x68k_header_t *)file;

    int err = x68k_validate(hdr, size);
    if (err < 0)
        return err;

    (void)p;
    (void)path;
    (void)argv;

    /* TODO: Phase 1 Steps 4–8 implement the actual loader. */
    return -(int)ENOSYS;
}
