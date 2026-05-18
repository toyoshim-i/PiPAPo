/*
 * tmpfs.c — RAM-backed temporary filesystem
 *
 * Provides a volatile filesystem at /tmp.  File data is stored in pages
 * obtained from mod_core.mem_region_alloc(); metadata lives in a static inode
 * table.
 *
 * Design:
 *   - Flat inode table (TMPFS_INODE_MAX entries) with parent_ino linkage
 *   - Inode 0 is always the root directory
 *   - File data: one page (4 KB) per file, allocated on first write
 *   - Total data pages bounded by TMPFS_DATA_MAX / PAGE_SIZE
 *   - Lookup: linear scan for inodes matching parent_ino
 *   - No symbolic links (not needed for /tmp use cases)
 */

#include "kernel/vfs/tmpfs.h"

#include <stddef.h>

#include "common/errno.h"
#include "kernel/common/config.h"
#include "kernel/common/core/page_types.h"
#include "kernel/common/mod/mod_core.h"
#include "kernel/common/mod/mod_vfs.h"

/* ── Inode structure ──────────────────────────────────────────────────── */

typedef struct {
  char name[TMPFS_NAME_MAX + 1]; /* filename (NUL-terminated)             */
  uint8_t type;                  /* VNODE_FILE or VNODE_DIR                 */
  uint8_t active;                /* 1 = in use, 0 = free                    */
  uint32_t mode;                 /* S_IFREG|0644 or S_IFDIR|0755           */
  uint32_t size;                 /* data bytes (files only)                 */
  uint32_t parent_ino;           /* inode index of parent directory         */
  page_id_t data_page;           /* file data page id (PAGE_ID_INVALID = none)
                                  * — page_id_t is i16-safe; storing a void *
                                  * truncates above the 64 KB DS=0 window. */
  uint32_t mtime;                /* last modification time (epoch seconds) */
  uint32_t ctime;                /* last status change time                */
  uint32_t atime; /* last access time (0 — atime not tracked) */
} tmpfs_inode_t;

static tmpfs_inode_t inodes[TMPFS_INODE_MAX];
static uint32_t data_pages_used; /* pages allocated for file data   */

#define TMPFS_MAX_PAGES (TMPFS_DATA_MAX / PAGE_SIZE)
#define TMPFS_FILE_MAX PAGE_SIZE /* max bytes per file (one page)   */

/* ── String helpers (no libc) ─────────────────────────────────────────── */

static int str_eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (*a == '\0' && *b == '\0');
}

static void str_copy(char *dst, const char *src, uint32_t max) {
  uint32_t i = 0;
  while (i < max - 1 && src[i]) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

/* ── Inode management ─────────────────────────────────────────────────── */

static int inode_alloc(void) {
  /* Skip inode 0 (root) */
  for (int i = 1; i < TMPFS_INODE_MAX; i++)
    if (!inodes[i].active) return i;
  return -1;
}

static void inode_free(int ino) {
  if (inodes[ino].data_page != PAGE_ID_INVALID) {
    region_t r = {NULL, inodes[ino].data_page};
    mod_core.mem_region_free(PPAP_MEM_RAM_DATA, PAGE_SIZE, &r);
    data_pages_used--;
  }
  inodes[ino].active = 0;
  inodes[ino].data_page = PAGE_ID_INVALID;
  inodes[ino].size = 0;
}

/* ── VFS operations ───────────────────────────────────────────────────── */

static int tmpfs_mount(mount_entry_t *mnt, const void *dev_data) {
  (void)dev_data;

  /* Initialise all inodes */
  for (int i = 0; i < TMPFS_INODE_MAX; i++) {
    inodes[i].active = 0;
    inodes[i].data_page = PAGE_ID_INVALID;
  }
  data_pages_used = 0;

  /* Create root directory (inode 0) */
  uint32_t now = mod_core.time_now_sec();
  inodes[0].active = 1;
  inodes[0].type = VNODE_DIR;
  inodes[0].mode = S_IFDIR | 0755u;
  inodes[0].size = 0;
  inodes[0].parent_ino = 0;
  inodes[0].data_page = PAGE_ID_INVALID;
  inodes[0].mtime = now;
  inodes[0].ctime = now;
  inodes[0].atime = now;
  inodes[0].name[0] = '\0';

  /* Allocate root vnode */
  vnode_t *root = mod_vfs.vnode_alloc();
  if (!root) return -ENOMEM;

  root->type = VNODE_DIR;
  root->mode = S_IFDIR | 0755u;
  root->ino = 0;
  root->size = 0;
  root->mount = mnt;
  root->fs_priv = (void *)0;

  mnt->root = root;
  return 0;
}

static int tmpfs_lookup(vnode_t *dir, const char *name, vnode_t **result) {
  uint32_t dir_ino = dir->ino;

  for (int i = 0; i < TMPFS_INODE_MAX; i++) {
    if (!inodes[i].active) continue;
    if (inodes[i].parent_ino != dir_ino) continue;
    if (!str_eq(inodes[i].name, name)) continue;

    /* Found — create a vnode */
    vnode_t *vn = mod_vfs.vnode_alloc();
    if (!vn) return -ENOMEM;

    vn->ino = (uint32_t)i;
    vn->type = (vnode_type_t)inodes[i].type;
    vn->mode = inodes[i].mode;
    vn->size = inodes[i].size;
    vn->mount = dir->mount;
    vn->fs_priv = (void *)0;

    *result = vn;
    return 0;
  }

  return -ENOENT;
}

static long tmpfs_read(vnode_t *vn, page_id_t page, uint16_t page_off, size_t n,
                       uint32_t off) {
  if (vn->type == VNODE_DIR) return -(long)EISDIR;

  tmpfs_inode_t *ti = &inodes[vn->ino];

  if (off >= ti->size) return 0;
  if (off + n > ti->size) n = ti->size - off;

  if (ti->data_page != PAGE_ID_INVALID) {
    /* Page-to-page copy via a small kernel scratch buffer.  Going through
     * a kernel buffer keeps this i16-safe — the data page may live above
     * the 64 KB DS=0 window and is only reachable via mem_region_page_*. */
    uint8_t buf[64];
    uint16_t copied = 0;
    while (copied < (uint16_t)n) {
      uint16_t chunk = (uint16_t)n - copied;
      if (chunk > sizeof(buf)) chunk = sizeof(buf);
      mod_core.page_read(ti->data_page, (uint16_t)(off + copied), buf, chunk);
      mod_core.page_write(page, (uint16_t)(page_off + copied), buf, chunk);
      copied = (uint16_t)(copied + chunk);
    }
  } else {
    static const uint8_t zero_chunk[32];
    uint16_t zero_off = 0;

    while (zero_off < (uint16_t)n) {
      uint16_t zero_len = (uint16_t)n - zero_off;
      if (zero_len > sizeof(zero_chunk)) zero_len = sizeof(zero_chunk);
      mod_core.page_write(page, (uint16_t)(page_off + zero_off), zero_chunk,
                          zero_len);
      zero_off = (uint16_t)(zero_off + zero_len);
    }
  }

  return (long)n;
}

static long tmpfs_write(vnode_t *vn, page_id_t page, uint16_t page_off,
                        size_t n, uint32_t off) {
  if (vn->type == VNODE_DIR) return -(long)EISDIR;

  tmpfs_inode_t *ti = &inodes[vn->ino];
  uint32_t now = mod_core.time_now_sec();
  ti->mtime = now;
  ti->ctime = now;

  /* Enforce per-file limit (one page) */
  if (off + n > TMPFS_FILE_MAX) {
    if (off >= TMPFS_FILE_MAX) return -(long)ENOSPC;
    n = TMPFS_FILE_MAX - off;
  }

  /* Allocate data page on first write */
  if (ti->data_page == PAGE_ID_INVALID) {
    region_t r;

    if (data_pages_used >= TMPFS_MAX_PAGES) return -(long)ENOSPC;
    if (mod_core.mem_region_alloc(PPAP_MEM_RAM_DATA, PAGE_SIZE, 0, &r) < 0)
      return -(long)ENOMEM;
    ti->data_page = r.base_page;

    /* Zero the freshly allocated page via the page-indexed API.  Direct
     * memset on r.base would write through a 16-bit-truncated pointer on
     * i16 and corrupt arbitrary low memory. */
    mod_core.page_zero(ti->data_page, 0, PAGE_SIZE);
    data_pages_used++;
  }

  /* Page-to-page copy from user (page) to tmpfs storage (ti->data_page)
   * via a small kernel buffer — same i16 safety reasoning as in tmpfs_read. */
  {
    uint8_t buf[64];
    uint16_t copied = 0;
    while (copied < (uint16_t)n) {
      uint16_t chunk = (uint16_t)n - copied;
      if (chunk > sizeof(buf)) chunk = sizeof(buf);
      mod_core.page_read(page, (uint16_t)(page_off + copied), buf, chunk);
      mod_core.page_write(ti->data_page, (uint16_t)(off + copied), buf, chunk);
      copied = (uint16_t)(copied + chunk);
    }
  }

  /* Extend file size if needed */
  if (off + (uint32_t)n > ti->size) {
    ti->size = off + (uint32_t)n;
    vn->size = ti->size;
  }

  return (long)n;
}

static int tmpfs_readdir(vnode_t *dir, struct dirent *entries,
                         size_t max_entries, uint32_t *cookie) {
  uint32_t dir_ino = dir->ino;
  int count = 0;
  uint32_t skip = *cookie;
  uint32_t seen = 0;

  for (int i = 0; i < TMPFS_INODE_MAX && (size_t)count < max_entries; i++) {
    if (!inodes[i].active) continue;
    if (inodes[i].parent_ino != dir_ino) continue;
    /* Don't list root as its own child */
    if ((uint32_t)i == dir_ino) continue;

    if (seen < skip) {
      seen++;
      continue;
    }

    entries[count].d_ino = (uint32_t)i;
    entries[count].d_type = (inodes[i].type == VNODE_DIR) ? DT_DIR : DT_REG;
    str_copy(entries[count].d_name, inodes[i].name, VFS_NAME_MAX + 1);
    count++;
    seen++;
  }

  *cookie = seen;
  return count;
}

static int tmpfs_stat(vnode_t *vn, struct stat *st) {
  tmpfs_inode_t *ti = &inodes[vn->ino];

  st->st_ino = vn->ino;
  st->st_mode = ti->mode;
  st->st_nlink = 1;
  st->st_size = ti->size;
  st->st_mtime = ti->mtime;
  st->st_ctime = ti->ctime;
  st->st_atime = ti->atime;
  return 0;
}

static int tmpfs_create(vnode_t *dir, const char *name, uint32_t mode,
                        vnode_t **result) {
  /* Check for duplicate */
  for (int i = 0; i < TMPFS_INODE_MAX; i++) {
    if (inodes[i].active && inodes[i].parent_ino == dir->ino &&
        str_eq(inodes[i].name, name))
      return -EEXIST;
  }

  int idx = inode_alloc();
  if (idx < 0) return -ENOSPC;

  uint32_t now = mod_core.time_now_sec();
  tmpfs_inode_t *ti = &inodes[idx];
  ti->active = 1;
  ti->type = VNODE_FILE;
  ti->mode = S_IFREG | (mode & 0777u);
  ti->size = 0;
  ti->parent_ino = dir->ino;
  ti->data_page = PAGE_ID_INVALID;
  ti->mtime = now;
  ti->ctime = now;
  ti->atime = now;
  str_copy(ti->name, name, TMPFS_NAME_MAX + 1);

  /* Parent directory's mtime/ctime bump — the directory's contents changed. */
  inodes[dir->ino].mtime = now;
  inodes[dir->ino].ctime = now;

  /* Return vnode for the new file */
  vnode_t *vn = mod_vfs.vnode_alloc();
  if (!vn) {
    ti->active = 0;
    return -ENOMEM;
  }

  vn->ino = (uint32_t)idx;
  vn->type = VNODE_FILE;
  vn->mode = ti->mode;
  vn->size = 0;
  vn->mount = dir->mount;
  vn->fs_priv = (void *)0;

  *result = vn;
  return 0;
}

static int tmpfs_mkdir(vnode_t *dir, const char *name, uint32_t mode) {
  /* Check for duplicate */
  for (int i = 0; i < TMPFS_INODE_MAX; i++) {
    if (inodes[i].active && inodes[i].parent_ino == dir->ino &&
        str_eq(inodes[i].name, name))
      return -EEXIST;
  }

  int idx = inode_alloc();
  if (idx < 0) return -ENOSPC;

  uint32_t now = mod_core.time_now_sec();
  tmpfs_inode_t *ti = &inodes[idx];
  ti->active = 1;
  ti->type = VNODE_DIR;
  ti->mode = S_IFDIR | (mode & 0777u);
  ti->size = 0;
  ti->parent_ino = dir->ino;
  ti->data_page = PAGE_ID_INVALID;
  ti->mtime = now;
  ti->ctime = now;
  ti->atime = now;
  str_copy(ti->name, name, TMPFS_NAME_MAX + 1);

  inodes[dir->ino].mtime = now;
  inodes[dir->ino].ctime = now;

  return 0;
}

static int tmpfs_unlink(vnode_t *dir, const char *name) {
  for (int i = 0; i < TMPFS_INODE_MAX; i++) {
    if (!inodes[i].active) continue;
    if (inodes[i].parent_ino != dir->ino) continue;
    if (!str_eq(inodes[i].name, name)) continue;

    /* If directory, check it's empty */
    if (inodes[i].type == VNODE_DIR) {
      for (int j = 0; j < TMPFS_INODE_MAX; j++) {
        if (j == i) continue;
        if (inodes[j].active && inodes[j].parent_ino == (uint32_t)i)
          return -ENOTEMPTY;
      }
    }

    inode_free(i);
    uint32_t now = mod_core.time_now_sec();
    inodes[dir->ino].mtime = now;
    inodes[dir->ino].ctime = now;
    return 0;
  }

  return -ENOENT;
}

static int tmpfs_rename(vnode_t *old_dir, const char *old_name,
                        vnode_t *new_dir, const char *new_name) {
  /* Find the source inode */
  int src = -1;
  for (int i = 0; i < TMPFS_INODE_MAX; i++) {
    if (!inodes[i].active) continue;
    if (inodes[i].parent_ino != old_dir->ino) continue;
    if (!str_eq(inodes[i].name, old_name)) continue;
    src = i;
    break;
  }
  if (src < 0) return -ENOENT;

  /* Check if destination already exists */
  for (int i = 0; i < TMPFS_INODE_MAX; i++) {
    if (!inodes[i].active) continue;
    if (inodes[i].parent_ino != new_dir->ino) continue;
    if (!str_eq(inodes[i].name, new_name)) continue;

    /* Cannot overwrite a non-empty directory */
    if (inodes[i].type == VNODE_DIR) {
      for (int j = 0; j < TMPFS_INODE_MAX; j++) {
        if (j == i) continue;
        if (inodes[j].active && inodes[j].parent_ino == (uint32_t)i)
          return -ENOTEMPTY;
      }
    }
    /* Remove the existing target */
    inode_free(i);
    break;
  }

  /* Move: update parent and name */
  uint32_t now = mod_core.time_now_sec();
  inodes[src].parent_ino = new_dir->ino;
  inodes[src].ctime = now;
  str_copy(inodes[src].name, new_name, TMPFS_NAME_MAX + 1);

  inodes[old_dir->ino].mtime = now;
  inodes[old_dir->ino].ctime = now;
  if (new_dir->ino != old_dir->ino) {
    inodes[new_dir->ino].mtime = now;
    inodes[new_dir->ino].ctime = now;
  }
  return 0;
}

static int tmpfs_truncate(vnode_t *vn, uint32_t length) {
  tmpfs_inode_t *ti = &inodes[vn->ino];

  if (ti->type == VNODE_DIR) return -EISDIR;

  if (length == 0 && ti->data_page != PAGE_ID_INVALID) {
    region_t r = {NULL, ti->data_page};
    mod_core.mem_region_free(PPAP_MEM_RAM_DATA, PAGE_SIZE, &r);
    ti->data_page = PAGE_ID_INVALID;
    data_pages_used--;
  }

  ti->size = length;
  vn->size = length;
  uint32_t now = mod_core.time_now_sec();
  ti->mtime = now;
  ti->ctime = now;
  return 0;
}

/* ── tmpfs_statfs ─────────────────────────────────────────────────────── */

static int tmpfs_statfs(mount_entry_t *mnt, struct kernel_statfs *buf) {
  (void)mnt;
  __builtin_memset(buf, 0, sizeof(*buf));

  /* Count active inodes */
  uint32_t active = 0;
  for (int i = 0; i < TMPFS_INODE_MAX; i++)
    if (inodes[i].active) active++;

  buf->f_type = 0x01021994u; /* Linux TMPFS_MAGIC */
  buf->f_bsize = PAGE_SIZE;
  buf->f_frsize = PAGE_SIZE;
  buf->f_blocks = TMPFS_MAX_PAGES;
  buf->f_bfree = TMPFS_MAX_PAGES - data_pages_used;
  buf->f_bavail = TMPFS_MAX_PAGES - data_pages_used;
  buf->f_files = TMPFS_INODE_MAX;
  buf->f_ffree = TMPFS_INODE_MAX - active;
  buf->f_namelen = TMPFS_NAME_MAX;
  return 0;
}

static int tmpfs_utimes(vnode_t *vn, uint32_t atime, uint32_t mtime) {
  tmpfs_inode_t *ti = &inodes[vn->ino];
  ti->atime = atime;
  ti->mtime = mtime;
  ti->ctime = mod_core.time_now_sec();
  return 0;
}

static int tmpfs_chmod(vnode_t *vn, uint32_t mode) {
  tmpfs_inode_t *ti = &inodes[vn->ino];
  ti->mode = (ti->mode & S_IFMT) | (mode & 07777u);
  ti->ctime = mod_core.time_now_sec();
  vn->mode = ti->mode;
  return 0;
}

/* ── Operations table ─────────────────────────────────────────────────── */

const vfs_ops_t tmpfs_ops = {
    .mount = tmpfs_mount,
    .lookup = tmpfs_lookup,
    .read = tmpfs_read,
    .write = tmpfs_write,
    .readdir = tmpfs_readdir,
    .stat = tmpfs_stat,
    .readlink = (void *)0,
    .create = tmpfs_create,
    .mkdir = tmpfs_mkdir,
    .unlink = tmpfs_unlink,
    .truncate = tmpfs_truncate,
    .statfs = tmpfs_statfs,
    .rename = tmpfs_rename,
    .utimes = tmpfs_utimes,
    .chmod = tmpfs_chmod,
};
