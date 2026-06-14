/*
 * blkdev.c — Block device registry
 *
 * Manages a fixed-size table of registered block devices.  Filesystem
 * drivers look up devices by name; devices are published only during
 * bootstrap before sched_start().
 */

#include "kernel/vfs/driver/blkdev.h"

#include "common/errno.h"
#include "kernel/common/mod/mod_core.h"
#include "kernel/common/spinlock.h"

/* ── Registry ────────────────────────────────────────────────────────────── */

static blkdev_t blkdev_table[BLKDEV_MAX];

/* ── Shared sector cache ─────────────────────────────────────────────────── */

typedef struct {
  blkdev_t *dev;
  uint32_t sector;
  uint32_t age;
  uint8_t valid;
} blkdev_cache_entry_t;

static blkdev_cache_entry_t cache[BLKDEV_CACHE_SLOTS];
static page_id_t cache_page = PAGE_ID_INVALID;
static uint32_t cache_age;
static bool cache_enabled;
static uint8_t cache_buf[BLKDEV_SECTOR_SIZE]
    __attribute__((aligned(BLKDEV_SECTOR_SIZE)));

_Static_assert(BLKDEV_CACHE_SLOTS *BLKDEV_SECTOR_SIZE <= PAGE_SIZE,
               "blkdev cache payload must fit in one page");

/* Block I/O runs in process context, so the cache lock only needs
 * cross-core exclusion. */

static int cache_find_locked(blkdev_t *dev, uint32_t sector) {
  for (uint32_t i = 0; i < BLKDEV_CACHE_SLOTS; i++) {
    if (cache[i].valid && cache[i].dev == dev && cache[i].sector == sector)
      return (int)i;
  }
  return -1;
}

static uint32_t cache_victim_locked(void) {
  uint32_t victim = 0;
  uint32_t oldest = UINT32_MAX;

  for (uint32_t i = 0; i < BLKDEV_CACHE_SLOTS; i++) {
    if (!cache[i].valid) return i;
    if (cache[i].age < oldest) {
      oldest = cache[i].age;
      victim = i;
    }
  }
  return victim;
}

static void cache_invalidate_all_locked(void) {
  for (uint32_t i = 0; i < BLKDEV_CACHE_SLOTS; i++) cache[i].valid = 0u;
}

static int cache_read_hit(blkdev_t *dev, page_id_t page, uint16_t off,
                          uint32_t sector) {
  spin_lock(SPIN_BLKDEV);
  if (!cache_enabled || cache_page == PAGE_ID_INVALID) {
    spin_unlock(SPIN_BLKDEV);
    return 0;
  }

  int idx = cache_find_locked(dev, sector);

  if (idx >= 0) {
    cache[idx].age = ++cache_age;
    mod_core.page_read(cache_page, (uint16_t)(idx * BLKDEV_SECTOR_SIZE),
                       cache_buf, BLKDEV_SECTOR_SIZE);
    mod_core.page_write(page, off, cache_buf, BLKDEV_SECTOR_SIZE);
  }
  spin_unlock(SPIN_BLKDEV);
  return idx >= 0;
}

static void cache_insert_from_page(blkdev_t *dev, page_id_t page, uint16_t off,
                                   uint32_t sector) {
  spin_lock(SPIN_BLKDEV);
  if (!cache_enabled || cache_page == PAGE_ID_INVALID) {
    spin_unlock(SPIN_BLKDEV);
    return;
  }

  int idx = cache_find_locked(dev, sector);

  if (idx < 0) idx = (int)cache_victim_locked();

  cache[idx].dev = dev;
  cache[idx].sector = sector;
  cache[idx].age = ++cache_age;
  cache[idx].valid = 1u;
  mod_core.page_read(page, off, cache_buf, BLKDEV_SECTOR_SIZE);
  mod_core.page_write(cache_page, (uint16_t)(idx * BLKDEV_SECTOR_SIZE),
                      cache_buf, BLKDEV_SECTOR_SIZE);
  spin_unlock(SPIN_BLKDEV);
}

static void cache_invalidate(blkdev_t *dev, uint32_t sector) {
  spin_lock(SPIN_BLKDEV);
  if (!cache_enabled || cache_page == PAGE_ID_INVALID) {
    spin_unlock(SPIN_BLKDEV);
    return;
  }

  int idx = cache_find_locked(dev, sector);

  if (idx >= 0) cache[idx].valid = 0u;
  spin_unlock(SPIN_BLKDEV);
}

static int blkdev_cached_read(struct blkdev *dev, page_id_t page, uint16_t off,
                              uint32_t sector, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    uint32_t s = sector + i;
    uint16_t sector_off = (uint16_t)(off + i * BLKDEV_SECTOR_SIZE);

    if (cache_read_hit(dev, page, sector_off, s)) continue;

    int rc = dev->raw_read(dev, page, sector_off, s, 1);
    if (rc < 0) return rc;
    cache_insert_from_page(dev, page, sector_off, s);
  }
  return 0;
}

static int blkdev_cached_write(struct blkdev *dev, page_id_t page, uint16_t off,
                               uint32_t sector, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    uint32_t s = sector + i;
    uint16_t sector_off = (uint16_t)(off + i * BLKDEV_SECTOR_SIZE);

    cache_invalidate(dev, s);
    int rc = dev->raw_write(dev, page, sector_off, s, 1);
    if (rc < 0) return rc;
    cache_insert_from_page(dev, page, sector_off, s);
  }
  return 0;
}

/* ── String helpers (no libc) ────────────────────────────────────────────── */

static int str_eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (*a == '\0' && *b == '\0');
}

/* ── API ─────────────────────────────────────────────────────────────────── */

void blkdev_init(void) {
  for (int i = 0; i < BLKDEV_MAX; i++) blkdev_table[i].name = (const char *)0;
  cache_invalidate_all_locked();
  cache_page = PAGE_ID_INVALID;
  cache_age = 0u;
  cache_enabled = false;
}

void blkdev_cache_set_enabled(bool enabled) {
  if (enabled && cache_page == PAGE_ID_INVALID) {
    region_t r;
    if (mod_core.region_alloc(PPAP_MEM_RAM_DATA, PAGE_SIZE, 0, &r) == 0)
      cache_page = r.base_page;
  }

  spin_lock(SPIN_BLKDEV);
  cache_enabled = enabled && cache_page != PAGE_ID_INVALID;
  cache_invalidate_all_locked();
  spin_unlock(SPIN_BLKDEV);
}

int blkdev_register(const blkdev_t *dev) {
  if (!dev || !dev->name) return -EINVAL;

  for (int i = 0; i < BLKDEV_MAX; i++) {
    if (!blkdev_table[i].name) {
      blkdev_table[i] = *dev;
      blkdev_table[i].raw_read = dev->read;
      blkdev_table[i].raw_write = dev->write;
      if ((dev->flags & BLKDEV_F_NOCACHE) == 0u) {
        blkdev_table[i].read = blkdev_cached_read;
        blkdev_table[i].write = blkdev_cached_write;
      }
      return i;
    }
  }
  return -ENOMEM;
}

blkdev_t *blkdev_find(const char *name) {
  if (!name) return (blkdev_t *)0;

  for (int i = 0; i < BLKDEV_MAX; i++) {
    if (blkdev_table[i].name && str_eq(blkdev_table[i].name, name))
      return &blkdev_table[i];
  }
  return (blkdev_t *)0;
}
