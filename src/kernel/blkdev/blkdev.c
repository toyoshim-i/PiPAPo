/*
 * blkdev.c — Block device registry
 *
 * Manages a fixed-size table of registered block devices.  Filesystem
 * drivers look up devices by name; hardware drivers register at boot.
 */

#include "blkdev.h"
#include "../errno.h"

/* ── Registry ────────────────────────────────────────────────────────────── */

static blkdev_t blkdev_table[BLKDEV_MAX];

/* ── String helpers (no libc) ────────────────────────────────────────────── */

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0');
}

/* ── API ─────────────────────────────────────────────────────────────────── */

void blkdev_init(void)
{
    for (int i = 0; i < BLKDEV_MAX; i++)
        blkdev_table[i].name = (const char *)0;
}

int blkdev_register(const blkdev_t *dev)
{
    if (!dev || !dev->name)
        return -EINVAL;

    for (int i = 0; i < BLKDEV_MAX; i++) {
        if (!blkdev_table[i].name) {
            blkdev_table[i] = *dev;
            return i;
        }
    }
    return -ENOMEM;
}

blkdev_t *blkdev_find(const char *name)
{
    if (!name)
        return (blkdev_t *)0;

    for (int i = 0; i < BLKDEV_MAX; i++) {
        if (blkdev_table[i].name && str_eq(blkdev_table[i].name, name))
            return &blkdev_table[i];
    }
    return (blkdev_t *)0;
}
