/*
 * loopback.c — Loopback block device driver
 *
 * Translates block-level I/O (512-byte sectors) into VFS file I/O on an
 * underlying image file.  The loopback device operates at the vnode level,
 * calling the backing filesystem's ops->read / ops->write directly.
 * This keeps the I/O stack shallow and avoids re-entering the syscall path.
 *
 * I/O path: UFS → loop_read → vfat_ops.read → SD blkdev → SPI
 *
 * Each loopback slot holds a reference to the backing vnode (refcnt
 * incremented on setup, decremented on teardown).
 */

#include "kernel/vfs/driver/loopback.h"

#include <stddef.h>

#include "common/errno.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/common/subtle/mem_helper.h"
#include "kernel/vfs/driver/blkdev.h"

/* ── Loopback device state ─────────────────────────────────────────────── */

typedef struct {
  blkdev_t blk;       /* registered block device */
  vnode_t *backing;   /* backing vnode (VFAT image file) */
  uint32_t file_size; /* image file size in bytes */
  uint8_t active;     /* 1 = in use, 0 = free */
} loop_dev_t;

static loop_dev_t loop_devs[LOOP_MAX];

/* Static name strings for each loop device */
static const char *loop_names[LOOP_MAX] = {"loop0", "loop1", "loop2"};

/* ── Read / write handlers ─────────────────────────────────────────────── */

static int loop_read(struct blkdev *dev, void *buf, uint32_t sector,
                     uint32_t count) {
  loop_dev_t *loop = (loop_dev_t *)dev->priv;

  if (!loop->active) return -ENODEV;

  uint32_t offset = sector * BLKDEV_SECTOR_SIZE;
  uint32_t nbytes = count * BLKDEV_SECTOR_SIZE;

  if (offset + nbytes > loop->file_size) return -EIO;

  /* Read from the backing vnode via its filesystem's read op */
  vnode_t *vn = loop->backing;
  page_id_t page;
  uint16_t page_off;
  if (mem_region_ptr_ref(buf, &page, &page_off) < 0) return -EFAULT;
  long n = vn->mount->ops->read(vn, page, page_off, nbytes, offset);
  return (n == (long)nbytes) ? 0 : -EIO;
}

static int loop_write(struct blkdev *dev, const void *buf, uint32_t sector,
                      uint32_t count) {
  loop_dev_t *loop = (loop_dev_t *)dev->priv;

  if (!loop->active) return -ENODEV;

  uint32_t offset = sector * BLKDEV_SECTOR_SIZE;
  uint32_t nbytes = count * BLKDEV_SECTOR_SIZE;

  if (offset + nbytes > loop->file_size) return -EIO;

  /* Write to the backing vnode via its filesystem's write op */
  vnode_t *vn = loop->backing;
  if (!vn->mount->ops->write) return -EROFS;

  page_id_t page;
  uint16_t page_off;
  if (mem_region_ptr_ref(buf, &page, &page_off) < 0) return -EFAULT;
  long n = vn->mount->ops->write(vn, page, page_off, nbytes, offset);
  return (n == (long)nbytes) ? 0 : -EIO;
}

/* ── Public API ────────────────────────────────────────────────────────── */

void loopback_init(void) {
  for (int i = 0; i < LOOP_MAX; i++) {
    loop_devs[i].active = 0;
    loop_devs[i].backing = (vnode_t *)0;
  }
}

int loopback_setup(const char *image_path) {
  if (!image_path) return -EINVAL;

  /* Find a free slot */
  int idx = -1;
  for (int i = 0; i < LOOP_MAX; i++) {
    if (!loop_devs[i].active) {
      idx = i;
      break;
    }
  }
  if (idx < 0) return -ENOMEM;

  /* Resolve the image file path to a vnode */
  vnode_t *vn = (vnode_t *)0;
  int rc = mod_vfs.lookup(image_path, &vn);
  if (rc < 0) return rc;

  /* Must be a regular file */
  if (vn->type != VNODE_FILE) {
    mod_vfs.vnode_release(vn);
    return -EINVAL;
  }

  /* File size must be non-zero and sector-aligned */
  uint32_t fsize = vn->size;
  if (fsize == 0 || (fsize % BLKDEV_SECTOR_SIZE) != 0) {
    mod_vfs.vnode_release(vn);
    return -EINVAL;
  }

  /* Fill in the loop device */
  loop_dev_t *loop = &loop_devs[idx];
  loop->backing = vn; /* vfs_lookup already incremented refcnt */
  loop->file_size = fsize;

  loop->blk.name = loop_names[idx];
  loop->blk.sector_count = fsize / BLKDEV_SECTOR_SIZE;
  loop->blk.priv = loop; /* back-pointer for read/write handlers */
  loop->blk.read = loop_read;
  loop->blk.write = loop_write;

  /* Register as a block device */
  rc = blkdev_register(&loop->blk);
  if (rc < 0) {
    mod_vfs.vnode_release(vn);
    return rc;
  }

  loop->active = 1;
  return idx;
}

int loopback_teardown(int loop_index) {
  if (loop_index < 0 || loop_index >= LOOP_MAX) return -EINVAL;

  loop_dev_t *loop = &loop_devs[loop_index];
  if (!loop->active) return -EINVAL;

  /* Release the backing vnode */
  if (loop->backing) {
    mod_vfs.vnode_release(loop->backing);
    loop->backing = (vnode_t *)0;
  }

  loop->active = 0;

  /* Note: the blkdev registry slot remains occupied (no unregister API).
   * The loop_read/loop_write functions check loop->active and return
   * -ENODEV if the device has been torn down. */
  return 0;
}

int loopback_is_active(int loop_index) {
  if (loop_index < 0 || loop_index >= LOOP_MAX) return 0;
  return loop_devs[loop_index].active;
}
