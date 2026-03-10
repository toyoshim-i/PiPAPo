/*
 * exec_x68k.c — Human68k X-format binary loader
 *
 * Phase 1 Step 1: detection only.  The loader is a stub that returns -ENOSYS.
 * Subsequent steps add header parsing, relocation, PMB setup, and F-line
 * DOS call bridging.
 */

#include "exec_x68k.h"
#include "kernel/errno.h"

/* ── Detection ─────────────────────────────────────────────────────────────── */

int x68k_detect(const uint8_t *file, uint32_t size)
{
    if (size < 64)          /* X-format header is 64 bytes */
        return 0;
    return file[0] == X68K_MAGIC_0 && file[1] == X68K_MAGIC_1;
}

/* ── Loader stub ───────────────────────────────────────────────────────────── */

int exec_x68k(pcb_t *p, const uint8_t *file, uint32_t size,
              const char *path, const char *const *argv)
{
    (void)p;
    (void)file;
    (void)size;
    (void)path;
    (void)argv;

    /* TODO: Phase 1 Steps 2–8 implement the actual loader. */
    return -(int)ENOSYS;
}
