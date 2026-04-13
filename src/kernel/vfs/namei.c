/*
 * namei.c — Path resolution (name-to-inode lookup)
 *
 * vfs_lookup() resolves an absolute path to a vnode by walking each
 * component through the VFS layer.
 *
 * Algorithm:
 *   1. Normalize the path: resolve "." and ".." lexically, collapse
 *      consecutive '/' characters.  This avoids needing parent pointers
 *      on vnodes.
 *   2. Walk the normalized path component by component.  At each step:
 *      a. Check if the resolved path so far matches a mount point.
 *         If yes, switch to that mount's root vnode (mount crossing).
 *      b. Otherwise, call the current FS driver's lookup() to find the
 *         child vnode.
 *      c. If the child is a symlink, read its target, build a new
 *         absolute path, normalize it, and recurse (depth-limited).
 *   3. Return the final vnode with refcnt incremented for the caller.
 *
 * Mount point directories do NOT need to exist in the underlying FS —
 * the VFS intercepts them before calling lookup().  This lets romfs (/)
 * work without needing empty /dev or /proc directory entries.
 */

#include <stddef.h>
#include <string.h>

#include "common/errno.h"
#include "kernel/common/core/proc_info.h" /* current->cwd for relative path
                                             resolution */
#include "kernel/vfs/vfs.h"

/* ── Path normalization ─────────────────────────────────────────────────────
 */

/*
 * Normalize an absolute path: resolve "." and ".." lexically, collapse
 * consecutive '/' characters.  Does NOT follow symlinks — that happens
 * during the walk.
 *
 * Input:  path must start with '/'
 * Output: buf receives the normalized path (always starts with '/')
 *
 * `path` and `buf` may alias. In-place normalization is supported as long
 * as `bufsiz` describes the writable destination buffer.
 *
 * Returns length of normalized path (≥ 1), or negative errno.
 *
 * Examples:
 *   "/foo/bar"        → "/foo/bar"
 *   "/foo/../bar"     → "/bar"
 *   "/foo/./bar"      → "/foo/bar"
 *   "//foo///bar//"   → "/foo/bar"
 *   "/"               → "/"
 *   "/../foo"         → "/foo"     (can't go above root)
 */
static int path_normalize(const char *path, char *buf, int bufsiz) {
  if (!path || path[0] != '/' || bufsiz < 2) return -EINVAL;

  int len = 0;
  const char *p = path;

  while (*p) {
    /* Skip slashes */
    while (*p == '/') p++;
    if (!*p) break;

    /* Find end of component */
    const char *start = p;
    while (*p && *p != '/') p++;
    int clen = (int)(p - start);

    /* "." — skip */
    if (clen == 1 && start[0] == '.') continue;

    /* ".." — pop last component */
    if (clen == 2 && start[0] == '.' && start[1] == '.') {
      /* Remove trailing "/component" from buf */
      while (len > 0 && buf[len - 1] != '/') len--;
      if (len > 0) len--; /* remove the '/' itself */
      buf[len] = '\0';
      continue;
    }

    /* Component too long? */
    if (clen > VFS_NAME_MAX) return -ENAMETOOLONG;
    if (len + 1 + clen >= bufsiz) return -ENAMETOOLONG;

    /* Append "/component" */
    buf[len++] = '/';
    __builtin_memmove(buf + len, start, (size_t)clen);
    len += clen;
    /* Do NOT write buf[len]='\0' here — when path==buf (in-place
     * normalization) the NUL would clobber the '/' separator that
     * p currently points at, truncating the remaining path. */
  }

  buf[len] = '\0';

  /* Empty result means root */
  if (len == 0) {
    buf[0] = '/';
    buf[1] = '\0';
    len = 1;
  }

  return len;
}

/* ── Internal: check for exact mount point match ────────────────────────────
 */

/*
 * Check if `resolved` exactly matches a mount point.  Uses vfs_mount_find()
 * and verifies that the remainder is empty (exact match, not just a prefix).
 *
 * Returns the matching mount entry, or NULL if no exact match.
 */
static mount_entry_t *mount_at(const char *resolved) {
  const char *rem = NULL;
  mount_entry_t *mnt = vfs_mount_find(resolved, &rem);

  if (mnt && rem && *rem == '\0') return mnt;
  return NULL;
}

/* ── Internal: walk a normalized path ───────────────────────────────────────
 */

static int lookup_walk_flags(const char *path_in, vnode_t **result,
                             int symloop_in, int flags) {
  const char *normalized = path_in;
  int symloop = symloop_in;

  /* Pool-allocated buffers for the walk.  Freed on every return path
   * via WALK_RETURN.  Keeps large temporaries off the 1 KB kernel stack. */
  char *resolved = vfs_scratch_alloc();
  char *restart_path = vfs_scratch_alloc();
  char *comp = vfs_scratch_alloc();
  if (!resolved || !restart_path || !comp) {
    vfs_scratch_free(comp);
    vfs_scratch_free(restart_path);
    vfs_scratch_free(resolved);
    return -ENOMEM;
  }

#define WALK_RETURN(rc)             \
  do {                              \
    vfs_scratch_free(comp);         \
    vfs_scratch_free(restart_path); \
    vfs_scratch_free(resolved);     \
    return (rc);                    \
  } while (0)

restart:
  if (symloop >= VFS_SYMLOOP_MAX) WALK_RETURN(-ELOOP);

  /* Find the root mount */
  mount_entry_t *root_mnt = mount_at("/");
  if (!root_mnt || !root_mnt->root) WALK_RETURN(-ENOENT);

  /* Handle bare "/" */
  if (normalized[0] == '/' && normalized[1] == '\0') {
    vfs_vnode_acquire(root_mnt->root);
    *result = root_mnt->root;
    WALK_RETURN(0);
  }

  /*
   * Walk the path component by component.
   *
   * `cur` is the current directory vnode.  It starts as the root mount's
   * root vnode (not ref'd by us — the mount holds it).  After each FS
   * lookup, `cur` is replaced with the newly-allocated child vnode
   * (refcnt = 1 from vfs_vnode_alloc in the FS driver's lookup).
   *
   * `cur_from_lookup` tracks whether we own a reference to `cur` that
   * needs vfs_vnode_release() on cleanup.  Mount root vnodes are permanent
   * (owned by the mount entry), so we don't put them.
   */
  int rlen = 0;
  resolved[0] = '\0';

  vnode_t *cur = root_mnt->root;
  int cur_from_lookup = 0; /* 0 = mount root, 1 = from FS lookup */

  const char *p = normalized + 1; /* skip leading '/' */

  while (*p) {
    /* Skip slashes */
    while (*p == '/') p++;
    if (!*p) break;

    /* Current vnode must be a directory */
    if (cur->type != VNODE_DIR) {
      if (cur_from_lookup) vfs_vnode_release(cur);
      WALK_RETURN(-ENOTDIR);
    }

    /* Extract the next path component (comp is pool-allocated) */
    int i = 0;
    while (*p && *p != '/' && i < VFS_NAME_MAX) comp[i++] = *p++;
    if (*p && *p != '/') {
      /* Component exceeds VFS_NAME_MAX */
      if (cur_from_lookup) vfs_vnode_release(cur);
      WALK_RETURN(-ENAMETOOLONG);
    }
    comp[i] = '\0';

    /* Build the resolved path: append "/component" */
    if (rlen + 1 + i >= VFS_PATH_MAX) {
      if (cur_from_lookup) vfs_vnode_release(cur);
      WALK_RETURN(-ENAMETOOLONG);
    }
    resolved[rlen++] = '/';
    __builtin_memcpy(resolved + rlen, comp, (size_t)i);
    rlen += i;
    resolved[rlen] = '\0';

    /* ── Mount point check (before FS lookup) ─────────────────────
     * If the resolved path exactly matches a mount point, switch to
     * that mount's root vnode.  This means mount-point directories
     * do NOT need to exist in the underlying FS.
     */
    mount_entry_t *child_mnt = mount_at(resolved);
    if (child_mnt && child_mnt->root &&
        child_mnt != (cur_from_lookup ? cur->mount : root_mnt)) {
      /* Cross into the new mount */
      if (cur_from_lookup) vfs_vnode_release(cur);
      cur = child_mnt->root;
      cur_from_lookup = 0; /* mount root — don't put */
      continue;
    }

    /* ── FS lookup ────────────────────────────────────────────────
     * Ask the current directory's FS driver to find the child.
     * Use cur->mount when set (always valid for mount roots and
     * for vnodes returned by FS lookup); fall back to root_mnt
     * only for the initial root vnode before any mount crossing.
     */
    mount_entry_t *cur_mnt = cur->mount ? cur->mount : root_mnt;
    if (!cur_mnt || !cur_mnt->ops || !cur_mnt->ops->lookup) {
      if (cur_from_lookup) vfs_vnode_release(cur);
      WALK_RETURN(-ENOENT);
    }

    vnode_t *child = NULL;
    int err = cur_mnt->ops->lookup(cur, comp, &child);
    if (err) {
      if (cur_from_lookup) vfs_vnode_release(cur);
      WALK_RETURN(err);
    }

    /* Release the previous directory vnode (if we own it) */
    if (cur_from_lookup) vfs_vnode_release(cur);

    /* ── Symlink handling ─────────────────────────────────────────
     * Read the link target, build a new absolute path (incorporating
     * any remaining components after the symlink), normalize, and
     * restart the walk with incremented depth counter.
     */
    if (child->type == VNODE_SYMLINK) {
      /* NOFOLLOW: if this is the final component, return the
       * symlink vnode itself instead of following it */
      if (flags & VFS_LOOKUP_NOFOLLOW) {
        const char *trail = p;
        while (*trail == '/') trail++;
        if (*trail == '\0') {
          *result = child;
          WALK_RETURN(0);
        }
      }

      char *target = vfs_scratch_alloc();
      char *norm = vfs_scratch_alloc();

      if (!target || !norm) {
        vfs_scratch_free(norm);
        vfs_scratch_free(target);
        vfs_vnode_release(child);
        WALK_RETURN(-ENOMEM);
      }

      if (!child->mount || !child->mount->ops || !child->mount->ops->readlink) {
        vfs_vnode_release(child);
        vfs_scratch_free(norm);
        vfs_scratch_free(target);
        WALK_RETURN(-EINVAL);
      }
      long tlen = child->mount->ops->readlink(child, target, VFS_PATH_MAX - 1);
      vfs_vnode_release(child);
      if (tlen < 0) {
        vfs_scratch_free(norm);
        vfs_scratch_free(target);
        WALK_RETURN((int)tlen);
      }
      target[tlen] = '\0';

      /* Build the new path to resolve (reuse `target` via
       * intermediate `resolved` prefix for relative symlinks,
       * then normalize into `norm`). */
      int nlen = 0;

      if (target[0] == '/') {
        /* Absolute symlink: start from target directly */
        nlen = (int)tlen;
        /* target already holds the path */
      } else {
        /* Relative symlink: prepend parent directory.
         * Parent = resolved path with the last component removed.
         * We must shift target right to make room for the prefix. */
        int parent_len = rlen;
        while (parent_len > 0 && resolved[parent_len - 1] != '/') parent_len--;
        if (parent_len == 0) parent_len = 1; /* root directory: "/" */

        if (parent_len + (int)tlen >= VFS_PATH_MAX) {
          vfs_scratch_free(norm);
          vfs_scratch_free(target);
          WALK_RETURN(-ENAMETOOLONG);
        }
        /* Shift target right to make room for parent prefix */
        __builtin_memmove(target + parent_len, target, (size_t)tlen + 1);
        if (parent_len == 1 && rlen == 0) {
          target[0] = '/';
        } else {
          __builtin_memcpy(target, resolved, (size_t)parent_len);
        }
        nlen = parent_len + (int)tlen;
      }

      /* Append any remaining path components after the symlink */
      if (*p) {
        int rem_len = (int)__builtin_strlen(p);
        if (nlen + 1 + rem_len >= VFS_PATH_MAX) {
          vfs_scratch_free(norm);
          vfs_scratch_free(target);
          WALK_RETURN(-ENAMETOOLONG);
        }
        target[nlen++] = '/';
        __builtin_memcpy(target + nlen, p, (size_t)rem_len);
        nlen += rem_len;
      }
      target[nlen] = '\0';

      /* Normalize (resolve any . / .. introduced by the target) */
      int norm_len = path_normalize(target, norm, VFS_PATH_MAX);
      if (norm_len < 0) {
        vfs_scratch_free(norm);
        vfs_scratch_free(target);
        WALK_RETURN(norm_len);
      }

      /* Copy normalized path to restart buffer, free symlink
       * buffers, and restart the walk iteratively. */
      __builtin_memcpy(restart_path, norm, (size_t)norm_len + 1);
      vfs_scratch_free(norm);
      vfs_scratch_free(target);
      normalized = restart_path;
      symloop++;
      goto restart;
    }

    /* Regular file or directory — advance */
    cur = child;
    cur_from_lookup = 1;
  }

  /* Reached the end of the path — `cur` is the result */
  if (!cur_from_lookup) {
    /* Result is a mount root — add a reference for the caller */
    vfs_vnode_acquire(cur);
  }
  /* else: cur has refcnt = 1 from the last FS lookup — caller owns it */

  *result = cur;
  WALK_RETURN(0);
#undef WALK_RETURN
}

/* ── Public API ─────────────────────────────────────────────────────────────
 */

int vfs_lookup(const char *path, vnode_t **result) {
  return vfs_lookup_flags(path, result, 0);
}

int vfs_lookup_flags(const char *path, vnode_t **result, int flags) {
  if (!path || !result) return -EINVAL;

  /* Resolve relative paths against current->cwd and normalize. */
  char *normalized = vfs_scratch_alloc();
  if (!normalized) return -ENOMEM;
  int nlen;

  if (path[0] != '/') {
    const char *cwd = (current && current->cwd[0]) ? current->cwd : "/";
    int cwdlen = (int)strlen(cwd);
    int pathlen = (int)strlen(path);

    if (cwdlen == 1) {
      if (1 + pathlen >= VFS_PATH_MAX) {
        vfs_scratch_free(normalized);
        return -ENAMETOOLONG;
      }
      normalized[0] = '/';
      memcpy(normalized + 1, path, (size_t)pathlen + 1);
    } else {
      if (cwdlen + 1 + pathlen >= VFS_PATH_MAX) {
        vfs_scratch_free(normalized);
        return -ENAMETOOLONG;
      }
      memcpy(normalized, cwd, (size_t)cwdlen);
      normalized[cwdlen] = '/';
      memcpy(normalized + cwdlen + 1, path, (size_t)pathlen + 1);
    }

    nlen = path_normalize(normalized, normalized, VFS_PATH_MAX);
  } else {
    nlen = path_normalize(path, normalized, VFS_PATH_MAX);
  }

  if (nlen < 0) {
    vfs_scratch_free(normalized);
    return nlen;
  }

  int rc = lookup_walk_flags(normalized, result, 0, flags);
  vfs_scratch_free(normalized);
  return rc;
}

int vfs_path_normalize(const char *path, char *buf, int bufsiz) {
  return path_normalize(path, buf, bufsiz);
}

int vfs_lookup_parent(const char *path, vnode_t **parent, char *namebuf,
                      int namebuf_size) {
  if (!path || !parent || !namebuf || namebuf_size < 2) return -EINVAL;
  if (path[0] != '/') return -EINVAL;

  /* Normalize the full path. */
  char *normalized = vfs_scratch_alloc();
  if (!normalized) return -ENOMEM;
  int nlen = path_normalize(path, normalized, VFS_PATH_MAX);
  if (nlen < 0) {
    vfs_scratch_free(normalized);
    return nlen;
  }

  /* Cannot get parent of "/" */
  if (nlen == 1 && normalized[0] == '/') {
    vfs_scratch_free(normalized);
    return -EINVAL;
  }

  /* Split: find last '/' to separate parent path from final component */
  int last_slash = 0;
  for (int i = nlen - 1; i >= 0; i--) {
    if (normalized[i] == '/') {
      last_slash = i;
      break;
    }
  }

  /* Extract the final component name */
  const char *name = &normalized[last_slash + 1];
  int name_len = nlen - last_slash - 1;
  if (name_len <= 0 || name_len >= namebuf_size) {
    vfs_scratch_free(normalized);
    return -ENAMETOOLONG;
  }
  __builtin_memcpy(namebuf, name, (size_t)name_len);
  namebuf[name_len] = '\0';

  /* Build parent path in-place: truncate normalized at last_slash. */
  if (last_slash == 0) {
    normalized[0] = '/';
    normalized[1] = '\0';
  } else {
    normalized[last_slash] = '\0';
  }

  /* Look up the parent directory */
  int rc = lookup_walk_flags(normalized, parent, 0, 0);
  vfs_scratch_free(normalized);
  return rc;
}
