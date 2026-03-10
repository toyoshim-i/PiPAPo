/*
 * exec_x68k.c — Human68k X-format binary loader
 *
 * Phase 1 Steps 1–2: detection and header validation.
 * Subsequent steps add relocation, PMB setup, and F-line DOS call bridging.
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

    /* TODO: Phase 1 Steps 3–8 implement the actual loader. */
    return -(int)ENOSYS;
}
