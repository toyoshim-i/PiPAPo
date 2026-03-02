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

#include "vfs.h"
#include "../errno.h"
#include <stddef.h>

/* ── Path normalization ───────────────────────────────────────────────────── */

/*
 * Normalize an absolute path: resolve "." and ".." lexically, collapse
 * consecutive '/' characters.  Does NOT follow symlinks — that happens
 * during the walk.
 *
 * Input:  path must start with '/'
 * Output: buf receives the normalized path (always starts with '/')
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
static int path_normalize(const char *path, char *buf, int bufsiz)
{
    if (!path || path[0] != '/' || bufsiz < 2)
        return -EINVAL;

    int len = 0;
    const char *p = path;

    while (*p) {
        /* Skip slashes */
        while (*p == '/')
            p++;
        if (!*p)
            break;

        /* Find end of component */
        const char *start = p;
        while (*p && *p != '/')
            p++;
        int clen = (int)(p - start);

        /* "." — skip */
        if (clen == 1 && start[0] == '.')
            continue;

        /* ".." — pop last component */
        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            /* Remove trailing "/component" from buf */
            while (len > 0 && buf[len - 1] != '/')
                len--;
            if (len > 0)
                len--;   /* remove the '/' itself */
            buf[len] = '\0';
            continue;
        }

        /* Component too long? */
        if (clen > VFS_NAME_MAX)
            return -ENAMETOOLONG;
        if (len + 1 + clen >= bufsiz)
            return -ENAMETOOLONG;

        /* Append "/component" */
        buf[len++] = '/';
        __builtin_memcpy(buf + len, start, (size_t)clen);
        len += clen;
        buf[len] = '\0';
    }

    /* Empty result means root */
    if (len == 0) {
        buf[0] = '/';
        buf[1] = '\0';
        len = 1;
    }

    return len;
}

/* ── Internal: check for exact mount point match ──────────────────────────── */

/*
 * Check if `resolved` exactly matches a mount point.  Uses vfs_find_mount()
 * and verifies that the remainder is empty (exact match, not just a prefix).
 *
 * Returns the matching mount entry, or NULL if no exact match.
 */
static mount_entry_t *mount_at(const char *resolved)
{
    const char *rem = NULL;
    mount_entry_t *mnt = vfs_find_mount(resolved, &rem);

    if (mnt && rem && *rem == '\0')
        return mnt;
    return NULL;
}

/* ── Internal: walk a normalized path ─────────────────────────────────────── */

static int lookup_walk(const char *normalized, vnode_t **result, int symloop)
{
    if (symloop >= VFS_SYMLOOP_MAX)
        return -ELOOP;

    /* Find the root mount */
    mount_entry_t *root_mnt = mount_at("/");
    if (!root_mnt || !root_mnt->root)
        return -ENOENT;

    /* Handle bare "/" */
    if (normalized[0] == '/' && normalized[1] == '\0') {
        vnode_ref(root_mnt->root);
        *result = root_mnt->root;
        return 0;
    }

    /*
     * Walk the path component by component.
     *
     * `cur` is the current directory vnode.  It starts as the root mount's
     * root vnode (not ref'd by us — the mount holds it).  After each FS
     * lookup, `cur` is replaced with the newly-allocated child vnode
     * (refcnt = 1 from vnode_alloc in the FS driver's lookup).
     *
     * `cur_from_lookup` tracks whether we own a reference to `cur` that
     * needs vnode_put() on cleanup.  Mount root vnodes are permanent
     * (owned by the mount entry), so we don't put them.
     */
    char resolved[VFS_PATH_MAX];   /* absolute path built so far */
    int rlen = 0;
    resolved[0] = '\0';

    vnode_t *cur = root_mnt->root;
    int cur_from_lookup = 0;       /* 0 = mount root, 1 = from FS lookup */

    const char *p = normalized + 1;   /* skip leading '/' */

    while (*p) {
        /* Skip slashes */
        while (*p == '/')
            p++;
        if (!*p)
            break;

        /* Current vnode must be a directory */
        if (cur->type != VNODE_DIR) {
            if (cur_from_lookup)
                vnode_put(cur);
            return -ENOTDIR;
        }

        /* Extract the next path component */
        char comp[VFS_NAME_MAX + 1];
        int i = 0;
        while (*p && *p != '/' && i < VFS_NAME_MAX)
            comp[i++] = *p++;
        if (*p && *p != '/') {
            /* Component exceeds VFS_NAME_MAX */
            if (cur_from_lookup)
                vnode_put(cur);
            return -ENAMETOOLONG;
        }
        comp[i] = '\0';

        /* Build the resolved path: append "/component" */
        if (rlen + 1 + i >= VFS_PATH_MAX) {
            if (cur_from_lookup)
                vnode_put(cur);
            return -ENAMETOOLONG;
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
            if (cur_from_lookup)
                vnode_put(cur);
            cur = child_mnt->root;
            cur_from_lookup = 0;   /* mount root — don't put */
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
            if (cur_from_lookup)
                vnode_put(cur);
            return -ENOENT;
        }

        vnode_t *child = NULL;
        int err = cur_mnt->ops->lookup(cur, comp, &child);
        if (err) {
            if (cur_from_lookup)
                vnode_put(cur);
            return err;
        }

        /* Release the previous directory vnode (if we own it) */
        if (cur_from_lookup)
            vnode_put(cur);

        /* ── Symlink handling ─────────────────────────────────────────
         * Read the link target, build a new absolute path (incorporating
         * any remaining components after the symlink), normalize, and
         * recurse with incremented depth counter.
         */
        if (child->type == VNODE_SYMLINK) {
            char target[VFS_PATH_MAX];

            if (!child->mount || !child->mount->ops ||
                !child->mount->ops->readlink) {
                vnode_put(child);
                return -EINVAL;
            }
            long tlen = child->mount->ops->readlink(
                child, target, VFS_PATH_MAX - 1);
            vnode_put(child);
            if (tlen < 0)
                return (int)tlen;
            target[tlen] = '\0';

            /* Build the new path to resolve */
            char new_path[VFS_PATH_MAX];
            int nlen = 0;

            if (target[0] == '/') {
                /* Absolute symlink: use target directly */
                __builtin_memcpy(new_path, target, (size_t)tlen);
                nlen = (int)tlen;
            } else {
                /* Relative symlink: resolve from parent directory.
                 * Parent = resolved path with the last component removed. */
                int parent_len = rlen;
                while (parent_len > 0 && resolved[parent_len - 1] != '/')
                    parent_len--;
                /* parent_len now points just past the last '/'.
                 * Keep the '/' to form "/parent/" */
                if (parent_len == 0) {
                    /* Symlink in the root directory */
                    new_path[0] = '/';
                    nlen = 1;
                } else {
                    __builtin_memcpy(new_path, resolved, (size_t)parent_len);
                    nlen = parent_len;
                }
                /* Append the symlink target */
                if (nlen + (int)tlen >= VFS_PATH_MAX)
                    return -ENAMETOOLONG;
                __builtin_memcpy(new_path + nlen, target, (size_t)tlen);
                nlen += (int)tlen;
            }

            /* Append any remaining path components after the symlink */
            if (*p) {
                int rem_len = (int)__builtin_strlen(p);
                if (nlen + 1 + rem_len >= VFS_PATH_MAX)
                    return -ENAMETOOLONG;
                new_path[nlen++] = '/';
                __builtin_memcpy(new_path + nlen, p, (size_t)rem_len);
                nlen += rem_len;
            }
            new_path[nlen] = '\0';

            /* Normalize (resolve any . / .. introduced by the target)
             * and recurse */
            char norm[VFS_PATH_MAX];
            int norm_len = path_normalize(new_path, norm, (int)sizeof(norm));
            if (norm_len < 0)
                return norm_len;

            return lookup_walk(norm, result, symloop + 1);
        }

        /* Regular file or directory — advance */
        cur = child;
        cur_from_lookup = 1;
    }

    /* Reached the end of the path — `cur` is the result */
    if (!cur_from_lookup) {
        /* Result is a mount root — add a reference for the caller */
        vnode_ref(cur);
    }
    /* else: cur has refcnt = 1 from the last FS lookup — caller owns it */

    *result = cur;
    return 0;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int vfs_lookup(const char *path, vnode_t **result)
{
    if (!path || !result)
        return -EINVAL;
    if (path[0] != '/')
        return -EINVAL;

    /* Normalize the path (resolve . and .. lexically) */
    char normalized[VFS_PATH_MAX];
    int nlen = path_normalize(path, normalized, (int)sizeof(normalized));
    if (nlen < 0)
        return nlen;

    return lookup_walk(normalized, result, 0);
}

int vfs_path_normalize(const char *path, char *buf, int bufsiz)
{
    return path_normalize(path, buf, bufsiz);
}

int vfs_lookup_parent(const char *path, vnode_t **parent,
                      char *namebuf, int namebuf_size)
{
    if (!path || !parent || !namebuf || namebuf_size < 2)
        return -EINVAL;
    if (path[0] != '/')
        return -EINVAL;

    /* Normalize the full path */
    char normalized[VFS_PATH_MAX];
    int nlen = path_normalize(path, normalized, (int)sizeof(normalized));
    if (nlen < 0)
        return nlen;

    /* Cannot get parent of "/" */
    if (nlen == 1 && normalized[0] == '/')
        return -EINVAL;

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
    if (name_len <= 0 || name_len >= namebuf_size)
        return -ENAMETOOLONG;
    __builtin_memcpy(namebuf, name, (size_t)name_len);
    namebuf[name_len] = '\0';

    /* Build parent path: everything up to last_slash, or "/" if at root */
    char parent_path[VFS_PATH_MAX];
    if (last_slash == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        __builtin_memcpy(parent_path, normalized, (size_t)last_slash);
        parent_path[last_slash] = '\0';
    }

    /* Look up the parent directory */
    return lookup_walk(parent_path, parent, 0);
}
