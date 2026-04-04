/*
 * devfs.c — Device pseudo-filesystem driver
 *
 * Implements vfs_ops_t for a RAM-resident device filesystem mounted at /dev.
 * All entries are created from a static table at mount time — there is no
 * on-disk backing.
 *
 * Devices in Phase 2:
 *   /dev/null    — read → EOF; write → discard
 *   /dev/zero    — read → zero-filled buffer; write → discard
 *   /dev/ttyS0   — read/write → existing UART tty driver
 *   /dev/urandom — read → RP2040 ROSC random bits (LFSR fallback on QEMU)
 */

#include "devfs.h"

#include "../common/mod/mod_vfs.h"
#ifdef PPAP_HAS_BLKDEV
#include "../core/driver/blkdev.h"
#include "../core/driver/loopback.h"
#endif
#include <stddef.h>
#include <stdint.h>

#include "../common/errno.h"
#include "../core/mm/mem_region.h"
#include "core/driver/uart.h"

/* ── Device node descriptor ──────────────────────────────────────────────── */

typedef struct {
  const char *name;
  long (*read)(page_id_t page, uint16_t page_off, size_t n, uint32_t off);
  long (*write)(page_id_t page, uint16_t page_off, size_t n, uint32_t off);
} devfs_node_t;

/* ── /dev/null ──────────────────────────────────────────────────────────────
 */

static long devnull_read(page_id_t page, uint16_t page_off, size_t n,
                         uint32_t off) {
  (void)page;
  (void)page_off;
  (void)n;
  (void)off;
  return 0; /* EOF */
}

static long devnull_write(page_id_t page, uint16_t page_off, size_t n,
                          uint32_t off) {
  (void)page;
  (void)page_off;
  (void)off;
  return (long)n; /* discard, report success */
}

/* ── /dev/zero ──────────────────────────────────────────────────────────────
 */

static long devzero_read(page_id_t page, uint16_t page_off, size_t n,
                         uint32_t off) {
  (void)off;
  static const uint8_t zero_chunk[32];
  size_t remaining = n;

  while (remaining > 0) {
    uint16_t chunk = mem_region_page_chunk_len(page_off, remaining);
    uint16_t written = 0;

    while (written < chunk) {
      uint16_t zero_len = chunk - written;
      if (zero_len > sizeof(zero_chunk)) zero_len = sizeof(zero_chunk);
      mem_region_page_write(page, (uint16_t)(page_off + written), zero_chunk,
                            zero_len);
      written = (uint16_t)(written + zero_len);
    }
    remaining -= chunk;
    mem_region_page_advance(&page, &page_off, chunk);
  }
  return (long)n;
}

/* ── /dev/ttyS0 ─────────────────────────────────────────────────────────────
 */

static long devtty_read(page_id_t page, uint16_t page_off, size_t n,
                        uint32_t off) {
  (void)off;
  size_t count = 0;
  while (count < n) {
    int c = uart_getc();
    if (c < 0) break; /* no more data available */
    uint8_t ch = (uint8_t)c;
    mem_region_page_write(page, page_off, &ch, 1);
    count++;
    mem_region_page_advance(&page, &page_off, 1);
  }
  return (long)count;
}

static long devtty_write(page_id_t page, uint16_t page_off, size_t n,
                         uint32_t off) {
  (void)off;
  for (size_t i = 0; i < n; i++) {
    uint8_t ch;
    mem_region_page_read(page, page_off, &ch, 1);
    uart_putc((char)ch, NULL);
    mem_region_page_advance(&page, &page_off, 1);
  }
  return (long)n;
}

/* ── /dev/urandom ───────────────────────────────────────────────────────────
 */

/* Simple LFSR for QEMU / m68k fallback (no ROSC hardware) */
static uint32_t lfsr_state = 0xDEADBEEFu;

#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
/* RP2040 ROSC random bit register (§4.7):
 *   ROSC_BASE = 0x40060000, RANDOMBIT offset = 0x1C
 *   Each read returns a single random bit in bit 0. */
#define ROSC_RANDOMBIT (*(volatile uint32_t *)0x4006001Cu)

/* QEMU detection: SCB.CPUID PARTNO (same check as smp.c) */
#define SCB_CPUID (*(volatile uint32_t *)0xE000ED00u)
#define CPUID_PARTNO_MASK 0x0000FFF0u
#define CPUID_PARTNO_M0P 0x0000C600u
#endif

static uint8_t random_byte(void) {
#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
  if ((SCB_CPUID & CPUID_PARTNO_MASK) == CPUID_PARTNO_M0P) {
    /* Real RP2040: collect 8 random bits from ROSC */
    uint8_t byte = 0;
    for (int b = 0; b < 8; b++)
      byte = (uint8_t)((byte << 1) | (ROSC_RANDOMBIT & 1u));
    return byte;
  }
#endif

  /* QEMU / m68k fallback: 32-bit Galois LFSR with taps at 32,22,2,1 */
  uint32_t bit = ((lfsr_state >> 0) ^ (lfsr_state >> 1) ^ (lfsr_state >> 21) ^
                  (lfsr_state >> 31)) &
                 1u;
  lfsr_state = (lfsr_state >> 1) | (bit << 31);
  return (uint8_t)(lfsr_state & 0xFFu);
}

static long devrandom_read(page_id_t page, uint16_t page_off, size_t n,
                           uint32_t off) {
  (void)off;
  for (size_t i = 0; i < n; i++) {
    uint8_t ch = random_byte();
    mem_region_page_write(page, page_off, &ch, 1);
    mem_region_page_advance(&page, &page_off, 1);
  }
  return (long)n;
}

/* ── /dev/backlight ──────────────────────────────────────────────────────── */

static int (*bl_hw_get)(uint8_t *val); /* NULL = not available */
static int (*bl_hw_set)(uint8_t val);

void devfs_set_backlight(int (*get)(uint8_t *), int (*set)(uint8_t)) {
  bl_hw_get = get;
  bl_hw_set = set;
}

/* Read: returns ASCII decimal brightness + newline, e.g. "128\n" */
static long devbacklight_read(page_id_t page, uint16_t page_off, size_t n,
                              uint32_t off) {
  if (!bl_hw_get) return -(long)ENODEV;
  uint8_t val;
  if (bl_hw_get(&val) < 0) return -(long)EIO;
  /* Format as ASCII decimal */
  char tmp[5]; /* "255\n" max */
  int len = 0;
  if (val >= 100) tmp[len++] = (char)('0' + val / 100);
  if (val >= 10) tmp[len++] = (char)('0' + (val / 10) % 10);
  tmp[len++] = (char)('0' + val % 10);
  tmp[len++] = '\n';
  /* Handle offset (for sequential reads) */
  if (off >= (uint32_t)len) return 0;
  size_t remaining = (size_t)(len - (int)off);
  if (remaining > n) remaining = n;
  size_t total = remaining;

  while (remaining > 0) {
    uint16_t chunk = mem_region_page_chunk_len(page_off, remaining);
    mem_region_page_write(page, page_off, tmp + off, chunk);
    remaining -= chunk;
    off += chunk;
    mem_region_page_advance(&page, &page_off, chunk);
  }
  return (long)total;
}

/* Write: parse ASCII decimal 0–255, set brightness */
static long devbacklight_write(page_id_t page, uint16_t page_off, size_t n,
                               uint32_t off) {
  (void)off;
  if (!bl_hw_set) return -(long)ENODEV;
  uint32_t val = 0;
  int digits = 0;
  for (size_t i = 0; i < n; i++) {
    uint8_t ch;
    mem_region_page_read(page, page_off, &ch, 1);
    if (ch >= '0' && ch <= '9') {
      val = val * 10 + (uint32_t)(ch - '0');
      digits++;
    } else if (ch == '\n' || ch == '\r' || ch == ' ') {
      if (digits) break; /* trailing whitespace */
    } else {
      return -(long)EINVAL;
    }
    mem_region_page_advance(&page, &page_off, 1);
  }
  if (!digits || val > 255) return -(long)EINVAL;
  if (bl_hw_set((uint8_t)val) < 0) return -(long)EIO;
  return (long)n;
}

/* ── /dev/power ──────────────────────────────────────────────────────────── */

static int (*power_hw_off)(void);

void devfs_set_power(int (*off_fn)(void)) { power_hw_off = off_fn; }

/* Read: returns "on\n" — system is running */
static long devpower_read(page_id_t page, uint16_t page_off, size_t n,
                          uint32_t off) {
  const char *msg = "on\n";
  int len = 3;
  if (off >= (uint32_t)len) return 0;
  size_t remaining = (size_t)(len - (int)off);
  if (remaining > n) remaining = n;
  size_t total = remaining;

  while (remaining > 0) {
    uint16_t chunk = mem_region_page_chunk_len(page_off, remaining);
    mem_region_page_write(page, page_off, msg + off, chunk);
    remaining -= chunk;
    off += chunk;
    mem_region_page_advance(&page, &page_off, chunk);
  }
  return (long)total;
}

/* Write "off" or "0" to power down */
static long devpower_write(page_id_t page, uint16_t page_off, size_t n,
                           uint32_t off) {
  (void)off;
  if (!power_hw_off) return -(long)ENODEV;
  uint8_t p0 = 0, p1 = 0, p2 = 0;
  if (n >= 1) mem_region_page_read(page, page_off, &p0, 1);
  if (n >= 2) {
    page_id_t next_page = page;
    uint16_t next_off = page_off;
    mem_region_page_advance(&next_page, &next_off, 1);
    mem_region_page_read(next_page, next_off, &p1, 1);
  }
  if (n >= 3) {
    page_id_t next_page = page;
    uint16_t next_off = page_off;
    mem_region_page_advance(&next_page, &next_off, 2);
    mem_region_page_read(next_page, next_off, &p2, 1);
  }
  /* Accept "off", "off\n", "0", "0\n" */
  if ((n >= 3 && p0 == 'o' && p1 == 'f' && p2 == 'f') ||
      (n >= 1 && p0 == '0')) {
    power_hw_off();
    /* Should not return, but just in case: */
    return (long)n;
  }
  return -(long)EINVAL;
}

#ifdef PPAP_HAS_BLKDEV
/* ── /dev/mmcblk0 — raw block device ────────────────────────────────────────
 */

static long devblk_read(page_id_t page, uint16_t page_off, size_t n,
                        uint32_t off) {
  blkdev_t *bd = blkdev_find("mmcblk0");
  if (!bd) return -(long)ENOENT;

  /* Sector-aligned access only */
  if ((off % BLKDEV_SECTOR_SIZE) != 0 || (page_off % BLKDEV_SECTOR_SIZE) != 0 ||
      (n % BLKDEV_SECTOR_SIZE) != 0)
    return -(long)EINVAL;

  uint32_t sector = off / BLKDEV_SECTOR_SIZE;
  size_t remaining = n;

  while (remaining > 0) {
    uint16_t chunk = mem_region_page_chunk_len(page_off, remaining);
    uint32_t count = (uint32_t)chunk / BLKDEV_SECTOR_SIZE;
    int rc = bd->read(
        bd, (void *)(uintptr_t)(mem_region_page_linear(page) + page_off),
        sector, count);
    if (rc < 0) return (long)rc;
    remaining -= chunk;
    sector += count;
    mem_region_page_advance(&page, &page_off, chunk);
  }
  return (long)n;
}

static long devblk_write(page_id_t page, uint16_t page_off, size_t n,
                         uint32_t off) {
  blkdev_t *bd = blkdev_find("mmcblk0");
  if (!bd) return -(long)ENOENT;

  if ((off % BLKDEV_SECTOR_SIZE) != 0 || (page_off % BLKDEV_SECTOR_SIZE) != 0 ||
      (n % BLKDEV_SECTOR_SIZE) != 0)
    return -(long)EINVAL;

  uint32_t sector = off / BLKDEV_SECTOR_SIZE;
  size_t remaining = n;

  while (remaining > 0) {
    uint16_t chunk = mem_region_page_chunk_len(page_off, remaining);
    uint32_t count = (uint32_t)chunk / BLKDEV_SECTOR_SIZE;
    int rc = bd->write(
        bd, (const void *)(uintptr_t)(mem_region_page_linear(page) + page_off),
        sector, count);
    if (rc < 0) return (long)rc;
    remaining -= chunk;
    sector += count;
    mem_region_page_advance(&page, &page_off, chunk);
  }
  return (long)n;
}

/* ── /dev/loopN — loopback block devices ────────────────────────────────────
 */

static long devloop_read_n(int idx, page_id_t page, uint16_t page_off, size_t n,
                           uint32_t off) {
  if (!loopback_is_active(idx)) return -(long)ENODEV;

  static const char *names[] = {"loop0", "loop1", "loop2"};
  blkdev_t *bd = blkdev_find(names[idx]);
  if (!bd) return -(long)ENOENT;

  /* Sector-aligned access only */
  if ((off % BLKDEV_SECTOR_SIZE) != 0 || (page_off % BLKDEV_SECTOR_SIZE) != 0 ||
      (n % BLKDEV_SECTOR_SIZE) != 0)
    return -(long)EINVAL;

  uint32_t sector = off / BLKDEV_SECTOR_SIZE;
  size_t remaining = n;

  while (remaining > 0) {
    uint16_t chunk = mem_region_page_chunk_len(page_off, remaining);
    uint32_t count = (uint32_t)chunk / BLKDEV_SECTOR_SIZE;
    int rc = bd->read(
        bd, (void *)(uintptr_t)(mem_region_page_linear(page) + page_off),
        sector, count);
    if (rc < 0) return (long)rc;
    remaining -= chunk;
    sector += count;
    mem_region_page_advance(&page, &page_off, chunk);
  }
  return (long)n;
}

static long devloop_write_n(int idx, page_id_t page, uint16_t page_off, size_t n,
                            uint32_t off) {
  if (!loopback_is_active(idx)) return -(long)ENODEV;

  static const char *names[] = {"loop0", "loop1", "loop2"};
  blkdev_t *bd = blkdev_find(names[idx]);
  if (!bd) return -(long)ENOENT;

  if ((off % BLKDEV_SECTOR_SIZE) != 0 || (page_off % BLKDEV_SECTOR_SIZE) != 0 ||
      (n % BLKDEV_SECTOR_SIZE) != 0)
    return -(long)EINVAL;

  uint32_t sector = off / BLKDEV_SECTOR_SIZE;
  size_t remaining = n;

  while (remaining > 0) {
    uint16_t chunk = mem_region_page_chunk_len(page_off, remaining);
    uint32_t count = (uint32_t)chunk / BLKDEV_SECTOR_SIZE;
    int rc = bd->write(
        bd, (const void *)(uintptr_t)(mem_region_page_linear(page) + page_off),
        sector, count);
    if (rc < 0) return (long)rc;
    remaining -= chunk;
    sector += count;
    mem_region_page_advance(&page, &page_off, chunk);
  }
  return (long)n;
}

static long devloop0_read(page_id_t page, uint16_t page_off, size_t n,
                          uint32_t off) {
  return devloop_read_n(0, page, page_off, n, off);
}
static long devloop0_write(page_id_t page, uint16_t page_off, size_t n,
                           uint32_t off) {
  return devloop_write_n(0, page, page_off, n, off);
}
static long devloop1_read(page_id_t page, uint16_t page_off, size_t n,
                          uint32_t off) {
  return devloop_read_n(1, page, page_off, n, off);
}
static long devloop1_write(page_id_t page, uint16_t page_off, size_t n,
                           uint32_t off) {
  return devloop_write_n(1, page, page_off, n, off);
}
static long devloop2_read(page_id_t page, uint16_t page_off, size_t n,
                          uint32_t off) {
  return devloop_read_n(2, page, page_off, n, off);
}
static long devloop2_write(page_id_t page, uint16_t page_off, size_t n,
                           uint32_t off) {
  return devloop_write_n(2, page, page_off, n, off);
}
#endif /* PPAP_HAS_BLKDEV */

/* ── Device table ───────────────────────────────────────────────────────────
 */

static const devfs_node_t devfs_nodes[] = {
    {"null", devnull_read, devnull_write},
    {"zero", devzero_read, devnull_write},
    {"ttyS0", devtty_read, devtty_write},
    {"tty1", devtty_read, devtty_write},
    {"console", devtty_read, devtty_write},
    {"tty", devtty_read, devtty_write},
    {"urandom", devrandom_read, devnull_write},
    {"backlight", devbacklight_read, devbacklight_write},
    {"power", devpower_read, devpower_write},
#ifdef PPAP_HAS_BLKDEV
    {"mmcblk0", devblk_read, devblk_write},
    {"loop0", devloop0_read, devloop0_write},
    {"loop1", devloop1_read, devloop1_write},
    {"loop2", devloop2_read, devloop2_write},
#endif
};

#define DEVFS_NODE_COUNT \
  ((uint32_t)(sizeof(devfs_nodes) / sizeof(devfs_nodes[0])))

/* ── String comparison helper ───────────────────────────────────────────────
 */

static int str_eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (*a == '\0' && *b == '\0');
}

static uint32_t str_len(const char *s) {
  uint32_t len = 0;
  while (s[len]) len++;
  return len;
}

/* ── devfs_mount ────────────────────────────────────────────────────────────
 */

static int devfs_mount(mount_entry_t *mnt, const void *dev_data) {
  (void)dev_data;

  /* Allocate root vnode for /dev directory */
  vnode_t *root = mod_vfs.vnode_alloc();
  if (!root) return -ENOMEM;

  root->type = VNODE_DIR;
  root->mode = S_IFDIR | 0755u;
  root->ino = 0; /* root inode = 0 */
  root->size = DEVFS_NODE_COUNT;
  root->mount = mnt;

  mnt->root = root;
  return 0;
}

/* ── devfs_lookup ───────────────────────────────────────────────────────────
 */

static int devfs_lookup(vnode_t *dir, const char *name, vnode_t **result) {
  (void)dir;

  for (uint32_t i = 0; i < DEVFS_NODE_COUNT; i++) {
    if (str_eq(devfs_nodes[i].name, name)) {
      vnode_t *vn = mod_vfs.vnode_alloc();
      if (!vn) return -ENOMEM;

      vn->type = VNODE_DEV;
      vn->mode = S_IFCHR | 0666u;
      vn->ino = i + 1; /* 1-based inode (0 = root dir) */
      vn->size = 0;
      vn->mount = dir->mount;
      vn->fs_priv = (void *)&devfs_nodes[i];

      *result = vn;
      return 0;
    }
  }

  return -ENOENT;
}

/* ── devfs_read ─────────────────────────────────────────────────────────────
 */

static long devfs_read(vnode_t *vn, page_id_t page, uint16_t page_off,
                       size_t n, uint32_t off) {
  if (vn->type == VNODE_DIR) return -(long)EISDIR;

  const devfs_node_t *node = (const devfs_node_t *)vn->fs_priv;
  if (!node || !node->read) return -(long)EIO;

  return node->read(page, page_off, n, off);
}

/* ── devfs_write ────────────────────────────────────────────────────────────
 */

static long devfs_write(vnode_t *vn, page_id_t page, uint16_t page_off,
                        size_t n, uint32_t off) {
  if (vn->type == VNODE_DIR) return -(long)EISDIR;

  const devfs_node_t *node = (const devfs_node_t *)vn->fs_priv;
  if (!node || !node->write) return -(long)EIO;

  return node->write(page, page_off, n, off);
}

/* ── devfs_readdir ──────────────────────────────────────────────────────────
 */

static int devfs_readdir(vnode_t *dir, struct dirent *entries,
                         size_t max_entries, uint32_t *cookie) {
  (void)dir;

  uint32_t idx = *cookie;
  int count = 0;

  while (idx < DEVFS_NODE_COUNT && (size_t)count < max_entries) {
    const devfs_node_t *node = &devfs_nodes[idx];

    entries[count].d_ino = idx + 1;
    entries[count].d_type = DT_CHR;

    uint32_t nlen = str_len(node->name);
    if (nlen > VFS_NAME_MAX) nlen = VFS_NAME_MAX;
    __builtin_memcpy(entries[count].d_name, node->name, nlen);
    entries[count].d_name[nlen] = '\0';

    idx++;
    count++;
  }

  *cookie = idx;
  return count;
}

/* ── devfs_stat ─────────────────────────────────────────────────────────────
 */

static int devfs_stat(vnode_t *vn, struct stat *st) {
  st->st_ino = vn->ino;
  st->st_mode = vn->mode;
  st->st_nlink = 1;
  st->st_size = vn->size;
  return 0;
}

/* ── devfs_statfs ───────────────────────────────────────────────────────────
 */

static int devfs_statfs(mount_entry_t *mnt, struct kernel_statfs *buf) {
  (void)mnt;
  __builtin_memset(buf, 0, sizeof(*buf));

  buf->f_type = 0x1373u; /* Linux DEVFS_SUPER_MAGIC */
  buf->f_bsize = 4096u;
  buf->f_frsize = 4096u;
  buf->f_files = DEVFS_NODE_COUNT;
  buf->f_ffree = 0;
  buf->f_namelen = VFS_NAME_MAX;
  return 0;
}

/* ── Operations table ───────────────────────────────────────────────────────
 */

const vfs_ops_t devfs_ops = {
    .mount = devfs_mount,
    .lookup = devfs_lookup,
    .read = devfs_read,
    .write = devfs_write,
    .readdir = devfs_readdir,
    .stat = devfs_stat,
    .readlink = NULL, /* no symlinks in devfs */
    .statfs = devfs_statfs,
};
