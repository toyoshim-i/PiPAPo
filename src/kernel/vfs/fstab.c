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

#include "common/mod/mod_vfs.h"
#ifdef PPAP_HAS_BLKDEV
#include "core/driver/blkdev.h"
#include "core/driver/loopback.h"
#include "ufs.h"
#include "vfat.h"
#endif
#include <stddef.h>

#include "common/errno.h"
#include "common/mod/mod_core.h"
#include "core/mm/mem_region.h"
#include "devfs.h"
#include "procfs.h"
#include "romfs.h"
#include "tmpfs.h"

/* ── String helpers ───────────────────────────────────────────────────── */

static int str_eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static void str_copy(char *dst, const char *src, int maxlen) {
  int i = 0;
  while (src[i] && i < maxlen - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

/* ── Options parser ───────────────────────────────────────────────────── */

/* Parse comma-separated options string.  Sets flags and loop fields. */
static void parse_options(const char *opts, uint8_t *flags, uint8_t *loop) {
  *flags = 0;
  *loop = 0;

  while (*opts) {
    /* Find end of current option (comma or NUL) */
    const char *end = opts;
    while (*end && *end != ',') end++;
    int len = (int)(end - opts);

    if (len == 2 && opts[0] == 'r' && opts[1] == 'o')
      *flags |= MNT_RDONLY;
    else if (len == 4 && opts[0] == 'l' && opts[1] == 'o' && opts[2] == 'o' &&
             opts[3] == 'p')
      *loop = 1;
    /* "rw" and unknown options are silently ignored */

    opts = end;
    if (*opts == ',') opts++;
  }
}

/* ── fstab_parse ──────────────────────────────────────────────────────── */

int fstab_parse(fstab_entry_t *entries, int max_entries) {
  vnode_t *vn = NULL;
  int err = mod_vfs.lookup("/etc/fstab", &vn);
  if (err < 0) return err;

  /* Read entire fstab (expected to be small — well under 512 bytes) */
  char buf[512];
  page_id_t page;
  uint16_t page_off;
  if (mem_region_ptr_ref(buf, &page, &page_off) < 0) {
    mod_vfs.vnode_release(vn);
    return -EFAULT;
  }
  long n = mod_vfs.vnode_read(vn, page, page_off, sizeof(buf) - 1, 0);
  mod_vfs.vnode_release(vn);
  if (n <= 0) return 0;
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
      if (*p == ' ' || *p == '\t') *p++ = '\0'; /* NUL-terminate field */
    }

    /* Skip rest of line */
    while (*p && *p != '\n') p++;
    if (*p == '\n') {
      *p = '\0';
      p++;
    }

    if (nfields < 4) continue; /* malformed line — skip */

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

int fstab_mount_all(const fstab_entry_t *entries, int count) {
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
        mod_core.klogf("fstab: %s not found, skipping %s\n", e->device, e->mountpoint);
        continue;
      }
      dev_data = bd;
    } else if (str_eq(e->fstype, "ufs")) {
      ops = &ufs_ops;
      if (e->loop) {
        /* Loopback mount: set up loop device first */
        int loop_idx = loopback_setup(e->device);
        if (loop_idx < 0) {
          mod_core.klogf("fstab: loopback_setup(%s) failed, skipping %s\n", e->device,
                e->mountpoint);
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
      mod_core.klogf("fstab: unknown fstype '%s'\n", e->fstype);
      continue;
    }

    int rc = mod_vfs.mount(e->mountpoint, ops, e->flags, dev_data);
    if (rc == 0)
      mod_core.klogf("VFS: %s mounted at %s\n", e->fstype, e->mountpoint);
    else
      mod_core.klogf("VFS: mount %s failed\n", e->mountpoint);
  }

  return 0;
}
