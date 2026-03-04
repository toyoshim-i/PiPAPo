/*
 * fstab.c — Boot-time mount table parser
 *
 * Reads /etc/fstab from romfs and mounts filesystems in the specified
 * order.  Supports pseudo-FS mounts (devfs, procfs, tmpfs), block
 * device mounts (VFAT), and loopback mounts (UFS on image files).
 *
 * The parser is intentionally simple: 4 whitespace-separated fields
 * per line (device, mountpoint, fstype, options).  Lines starting
 * with '#' are comments, blank lines are skipped.
 */

#include "fstab.h"
#include "../vfs/vfs.h"
#ifdef PPAP_HAS_BLKDEV
#include "../blkdev/blkdev.h"
#include "../blkdev/loopback.h"
#include "vfat.h"
#include "ufs.h"
#endif
#include "../syscall/syscall.h"
#include "drivers/uart.h"
#include "romfs.h"
#include "devfs.h"
#include "procfs.h"
#include "tmpfs.h"
#include "../errno.h"
#include <stddef.h>

/* ── String helpers ───────────────────────────────────────────────────── */

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void str_copy(char *dst, const char *src, int maxlen)
{
    int i = 0;
    while (src[i] && i < maxlen - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ── Options parser ───────────────────────────────────────────────────── */

/* Parse comma-separated options string.  Sets flags and loop fields. */
static void parse_options(const char *opts, uint8_t *flags, uint8_t *loop)
{
    *flags = 0;
    *loop  = 0;

    while (*opts) {
        /* Find end of current option (comma or NUL) */
        const char *end = opts;
        while (*end && *end != ',') end++;
        int len = (int)(end - opts);

        if (len == 2 && opts[0] == 'r' && opts[1] == 'o')
            *flags |= MNT_RDONLY;
        else if (len == 4 && opts[0] == 'l' && opts[1] == 'o'
                          && opts[2] == 'o' && opts[3] == 'p')
            *loop = 1;
        /* "rw" and unknown options are silently ignored */

        opts = end;
        if (*opts == ',') opts++;
    }
}

/* ── fstab_parse ──────────────────────────────────────────────────────── */

int fstab_parse(fstab_entry_t *entries, int max_entries)
{
    long fd = sys_open("/etc/fstab", 0 /* O_RDONLY */, 0);
    if (fd < 0)
        return (int)fd;

    /* Read entire fstab (expected to be small — well under 512 bytes) */
    char buf[512];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';

    int count = 0;
    char *p = buf;

    while (*p && count < max_entries) {
        /* Skip leading whitespace */
        while (*p == ' ' || *p == '\t') p++;

        /* Skip comments and blank lines */
        if (*p == '#' || *p == '\n' || *p == '\0') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }

        /* Parse 4 fields: device mountpoint fstype options */
        char *fields[4];
        int nfields = 0;

        for (int f = 0; f < 4 && *p && *p != '\n'; f++) {
            /* Skip whitespace between fields */
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\n' || *p == '\0') break;

            fields[f] = p;
            nfields++;

            /* Advance to end of field */
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
            if (*p == ' ' || *p == '\t')
                *p++ = '\0';  /* NUL-terminate field */
        }

        /* Skip rest of line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') {
            *p = '\0';
            p++;
        }

        if (nfields < 4) continue;  /* malformed line — skip */

        fstab_entry_t *e = &entries[count];
        str_copy(e->device, fields[0], (int)sizeof(e->device));
        str_copy(e->mountpoint, fields[1], (int)sizeof(e->mountpoint));
        str_copy(e->fstype, fields[2], (int)sizeof(e->fstype));
        parse_options(fields[3], &e->flags, &e->loop);
        count++;
    }

    return count;
}

/* ── fstab_mount_all ──────────────────────────────────────────────────── */

int fstab_mount_all(const fstab_entry_t *entries, int count)
{
    for (int i = 0; i < count; i++) {
        const fstab_entry_t *e = &entries[i];
        const vfs_ops_t *ops = (const vfs_ops_t *)0;
        const void *dev_data = (const void *)0;

        /* Match fstype to ops table */
        if (str_eq(e->fstype, "devfs"))
            ops = &devfs_ops;
        else if (str_eq(e->fstype, "procfs"))
            ops = &procfs_ops;
        else if (str_eq(e->fstype, "tmpfs"))
            ops = &tmpfs_ops;
#ifdef PPAP_HAS_BLKDEV
        else if (str_eq(e->fstype, "vfat")) {
            ops = &vfat_ops;
            blkdev_t *bd = blkdev_find(e->device);
            if (!bd) {
                uart_puts("fstab: ");
                uart_puts(e->device);
                uart_puts(" not found, skipping ");
                uart_puts(e->mountpoint);
                uart_puts("\n");
                continue;
            }
            dev_data = bd;
        } else if (str_eq(e->fstype, "ufs")) {
            ops = &ufs_ops;
            if (e->loop) {
                /* Loopback mount: set up loop device first */
                int loop_idx = loopback_setup(e->device);
                if (loop_idx < 0) {
                    uart_puts("fstab: loopback_setup(");
                    uart_puts(e->device);
                    uart_puts(") failed, skipping ");
                    uart_puts(e->mountpoint);
                    uart_puts("\n");
                    continue;
                }
                char loop_name[8] = "loop0";
                loop_name[4] = (char)('0' + loop_idx);
                blkdev_t *bd = blkdev_find(loop_name);
                if (!bd) continue;
                dev_data = bd;
            } else {
                blkdev_t *bd = blkdev_find(e->device);
                if (!bd) continue;
                dev_data = bd;
            }
        }
#endif
        else {
            uart_puts("fstab: unknown fstype '");
            uart_puts(e->fstype);
            uart_puts("'\n");
            continue;
        }

        int rc = vfs_mount(e->mountpoint, ops, e->flags, dev_data);
        if (rc == 0) {
            uart_puts("VFS: ");
            uart_puts(e->fstype);
            uart_puts(" mounted at ");
            uart_puts(e->mountpoint);
            uart_puts("\n");
        } else {
            uart_puts("VFS: mount ");
            uart_puts(e->mountpoint);
            uart_puts(" failed\n");
        }
    }

    return 0;
}
