/*
 * procfs.c — Process information pseudo-filesystem driver
 *
 * Implements vfs_ops_t for a RAM-resident pseudo-filesystem mounted at /proc.
 * All file content is generated dynamically on each read — there is no
 * cached state.
 *
 * Entries:
 *   /proc/meminfo        — free pages, total pages, page size
 *   /proc/version        — kernel version string
 *   /proc/stat           — aggregate CPU jiffy counters
 *   /proc/uptime         — seconds since boot
 *   /proc/mounts         — mounted filesystems (device path fstype opts 0 0)
 *   /proc/<pid>/stat     — per-process stat line (Linux 52-field format)
 *   /proc/<pid>/cmdline  — NUL-terminated command name
 */

#include "procfs.h"
#include "romfs.h"
#include "devfs.h"
#include "tmpfs.h"
#ifdef PPAP_HAS_BLKDEV
#include "vfat.h"
#include "ufs.h"
#endif
#include "../vfs/vfs.h"
#include "../mm/page.h"
#include "../proc/proc.h"
#include "../proc/sched.h"
#include "../errno.h"
#include "config.h"
#include <stddef.h>
#include <stdint.h>

/* ── Minimal integer-to-string formatter ──────────────────────────────────── */

/* Write unsigned decimal to buf, return number of chars written.
 * buf must have room for at least 10 digits + NUL. */
static int fmt_u32(char *buf, uint32_t v)
{
    char tmp[12];
    int len = 0;

    if (v == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }

    while (v > 0) {
        tmp[len++] = (char)('0' + (v % 10));
        v /= 10;
    }

    /* Reverse into buf */
    for (int i = 0; i < len; i++)
        buf[i] = tmp[len - 1 - i];
    buf[len] = '\0';
    return len;
}

/* Append src to dst, return new position in dst */
static int fmt_append(char *dst, int pos, int max, const char *src)
{
    while (*src && pos < max - 1)
        dst[pos++] = *src++;
    dst[pos] = '\0';
    return pos;
}

static int fmt_append_u32(char *dst, int pos, int max, uint32_t v)
{
    char tmp[12];
    fmt_u32(tmp, v);
    return fmt_append(dst, pos, max, tmp);
}

static int fmt_append_i32(char *dst, int pos, int max, int32_t v)
{
    if (v < 0) {
        pos = fmt_append(dst, pos, max, "-");
        v = -v;
    }
    return fmt_append_u32(dst, pos, max, (uint32_t)v);
}

/* ── String helpers ───────────────────────────────────────────────────────── */

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0');
}

static uint32_t str_len(const char *s)
{
    uint32_t len = 0;
    while (s[len]) len++;
    return len;
}

/* Parse unsigned integer from string.  Returns -1 if not a valid number. */
static int32_t parse_uint(const char *s)
{
    if (*s == '\0') return -1;
    uint32_t v = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
    }
    return (int32_t)v;
}

/* ── Static node generators ──────────────────────────────────────────────── */

typedef struct {
    const char *name;
    int (*generate)(char *buf, int bufsiz);  /* fill buf, return length */
} procfs_node_t;

/* ── /proc/meminfo ────────────────────────────────────────────────────────── */

static int gen_meminfo(char *buf, int bufsiz)
{
    uint32_t free_pages = page_free_count();
    uint32_t total_kb = (PAGE_COUNT * PAGE_SIZE) / 1024u;
    uint32_t free_kb  = (free_pages * PAGE_SIZE) / 1024u;

    int pos = 0;
    pos = fmt_append(buf, pos, bufsiz, "MemTotal:    ");
    pos = fmt_append_u32(buf, pos, bufsiz, total_kb);
    pos = fmt_append(buf, pos, bufsiz, " kB\nMemFree:     ");
    pos = fmt_append_u32(buf, pos, bufsiz, free_kb);
    pos = fmt_append(buf, pos, bufsiz, " kB\nPageSize:   ");
    pos = fmt_append_u32(buf, pos, bufsiz, PAGE_SIZE);
    pos = fmt_append(buf, pos, bufsiz, " B\n");
    return pos;
}

/* ── /proc/version ────────────────────────────────────────────────────────── */

static const char version_str[] =
    "PicoPiAndPortable v0.3 (armv6m)\n";

static int gen_version(char *buf, int bufsiz)
{
    int len = 0;
    while (version_str[len]) len++;
    if (len > bufsiz - 1)
        len = bufsiz - 1;
    __builtin_memcpy(buf, version_str, (uint32_t)len);
    buf[len] = '\0';
    return len;
}

/* ── /proc/stat — aggregate CPU counters ──────────────────────────────────── */

static int gen_stat(char *buf, int bufsiz)
{
    /* Aggregate line: cpu <user> <nice> <system> <idle> 0 0 0 0 0 0 */
    int pos = 0;
    pos = fmt_append(buf, pos, bufsiz, "cpu ");
    pos = fmt_append_u32(buf, pos, bufsiz, cpu_user_ticks[0] + cpu_user_ticks[1]);
    pos = fmt_append(buf, pos, bufsiz, " 0 ");
    pos = fmt_append_u32(buf, pos, bufsiz, cpu_system_ticks[0] + cpu_system_ticks[1]);
    pos = fmt_append(buf, pos, bufsiz, " ");
    pos = fmt_append_u32(buf, pos, bufsiz, cpu_idle_ticks[0] + cpu_idle_ticks[1]);
    pos = fmt_append(buf, pos, bufsiz, " 0 0 0 0 0 0\n");

    /* Per-core lines: cpu0/cpu1 */
    for (int c = 0; c < 2; c++) {
        pos = fmt_append(buf, pos, bufsiz, "cpu");
        pos = fmt_append_u32(buf, pos, bufsiz, (uint32_t)c);
        pos = fmt_append(buf, pos, bufsiz, " ");
        pos = fmt_append_u32(buf, pos, bufsiz, cpu_user_ticks[c]);
        pos = fmt_append(buf, pos, bufsiz, " 0 ");
        pos = fmt_append_u32(buf, pos, bufsiz, cpu_system_ticks[c]);
        pos = fmt_append(buf, pos, bufsiz, " ");
        pos = fmt_append_u32(buf, pos, bufsiz, cpu_idle_ticks[c]);
        pos = fmt_append(buf, pos, bufsiz, " 0 0 0 0 0 0\n");
    }
    return pos;
}

/* ── /proc/uptime — seconds since boot ────────────────────────────────────── */

static int gen_uptime(char *buf, int bufsiz)
{
    uint32_t ticks = sched_get_ticks();
    uint32_t secs = ticks / PPAP_TICK_HZ;
    uint32_t hundredths = (ticks % PPAP_TICK_HZ) * 100 / PPAP_TICK_HZ;
    uint32_t total_idle = cpu_idle_ticks[0] + cpu_idle_ticks[1];
    uint32_t idle_secs = total_idle / PPAP_TICK_HZ;
    uint32_t idle_hund = (total_idle % PPAP_TICK_HZ) * 100 / PPAP_TICK_HZ;

    int pos = 0;
    pos = fmt_append_u32(buf, pos, bufsiz, secs);
    pos = fmt_append(buf, pos, bufsiz, ".");
    if (hundredths < 10) pos = fmt_append(buf, pos, bufsiz, "0");
    pos = fmt_append_u32(buf, pos, bufsiz, hundredths);
    pos = fmt_append(buf, pos, bufsiz, " ");
    pos = fmt_append_u32(buf, pos, bufsiz, idle_secs);
    pos = fmt_append(buf, pos, bufsiz, ".");
    if (idle_hund < 10) pos = fmt_append(buf, pos, bufsiz, "0");
    pos = fmt_append_u32(buf, pos, bufsiz, idle_hund);
    pos = fmt_append(buf, pos, bufsiz, "\n");
    return pos;
}

/* ── /proc/mounts — mounted filesystems ───────────────────────────────────── */

/* Map ops pointer → fstype name string */
static const char *ops_to_fstype(const vfs_ops_t *ops)
{
    if (ops == &romfs_ops)  return "romfs";
    if (ops == &devfs_ops)  return "devfs";
    if (ops == &procfs_ops) return "proc";
    if (ops == &tmpfs_ops)  return "tmpfs";
#ifdef PPAP_HAS_BLKDEV
    if (ops == &vfat_ops)   return "vfat";
    if (ops == &ufs_ops)    return "ufs";
#endif
    return "unknown";
}

static int gen_mounts(char *buf, int bufsiz)
{
    int pos = 0;

    for (uint32_t i = 0; i < VFS_MOUNT_MAX; i++) {
        const mount_entry_t *mnt = &vfs_mount_table[i];
        if (!mnt->active)
            continue;

        const char *fstype = ops_to_fstype(mnt->ops);

        /* device — use fstype as device name (like Linux pseudo-FS) */
        pos = fmt_append(buf, pos, bufsiz, fstype);
        pos = fmt_append(buf, pos, bufsiz, " ");
        /* mount point */
        pos = fmt_append(buf, pos, bufsiz, mnt->path);
        pos = fmt_append(buf, pos, bufsiz, " ");
        /* fstype */
        pos = fmt_append(buf, pos, bufsiz, fstype);
        pos = fmt_append(buf, pos, bufsiz, " ");
        /* options */
        pos = fmt_append(buf, pos, bufsiz,
                         (mnt->flags & MNT_RDONLY) ? "ro" : "rw");
        pos = fmt_append(buf, pos, bufsiz, " 0 0\n");
    }

    return pos;
}

/* ── Node table ───────────────────────────────────────────────────────────── */

static const procfs_node_t procfs_nodes[] = {
    { "meminfo", gen_meminfo },
    { "version", gen_version },
    { "stat",    gen_stat },
    { "uptime",  gen_uptime },
    { "mounts",  gen_mounts },
};

#define PROCFS_NODE_COUNT \
    ((uint32_t)(sizeof(procfs_nodes) / sizeof(procfs_nodes[0])))

/* ── Per-PID content generators ──────────────────────────────────────────── */

/* Map proc_state_t to Linux single-char state */
static char state_char(const pcb_t *p)
{
    if (p->is_idle)
        return 'I';   /* idle kernel thread */
    switch (p->state) {
    case PROC_RUNNABLE:  return 'R';
    case PROC_SLEEPING:  return 'S';
    case PROC_BLOCKED:   return 'S';
    case PROC_ZOMBIE:    return 'Z';
    default:             return '?';
    }
}

/* Calculate VSZ (virtual memory size) for a process in bytes */
static uint32_t proc_vsz(const pcb_t *p)
{
    uint32_t pages = 0;
    /* Stack page */
    if (p->stack_page) pages++;
    /* User data pages */
    for (int i = 0; i < USER_PAGES_MAX; i++) {
        if (p->user_pages[i]) pages++;
    }
    /* mmap pages */
    for (int i = 0; i < MMAP_REGIONS_MAX; i++) {
        if (p->mmap_regions[i].addr)
            pages += p->mmap_regions[i].pages;
    }
    return pages * PAGE_SIZE;
}

/*
 * /proc/<pid>/stat — Linux 52-field format.
 * busybox ps extracts: pid, comm, state, ppid, pgid, sid, tty_nr,
 * utime(14), stime(15), nice(19), start_time(22), vsize(23), rss(24).
 * Fields we don't track are filled with 0.
 */
static int gen_pid_stat(char *buf, int bufsiz, const pcb_t *p)
{
    int pos = 0;
    /* 1: pid */
    pos = fmt_append_i32(buf, pos, bufsiz, p->pid);
    pos = fmt_append(buf, pos, bufsiz, " (");
    /* 2: comm */
    pos = fmt_append(buf, pos, bufsiz, p->comm[0] ? p->comm : "?");
    pos = fmt_append(buf, pos, bufsiz, ") ");
    /* 3: state */
    char sc[2] = { state_char(p), '\0' };
    pos = fmt_append(buf, pos, bufsiz, sc);
    pos = fmt_append(buf, pos, bufsiz, " ");
    /* 4: ppid */
    pos = fmt_append_i32(buf, pos, bufsiz, p->ppid);
    pos = fmt_append(buf, pos, bufsiz, " ");
    /* 5: pgrp */
    pos = fmt_append_i32(buf, pos, bufsiz, p->pgid);
    pos = fmt_append(buf, pos, bufsiz, " ");
    /* 6: session */
    pos = fmt_append_i32(buf, pos, bufsiz, p->sid);
    /* 7: tty_nr  8: tpgid  9: flags  10: minflt  11: cminflt
     * 12: majflt  13: cmajflt */
    pos = fmt_append(buf, pos, bufsiz, " 0 0 0 0 0 0 0 ");
    /* 14: utime */
    pos = fmt_append_u32(buf, pos, bufsiz, p->utime);
    pos = fmt_append(buf, pos, bufsiz, " ");
    /* 15: stime */
    pos = fmt_append_u32(buf, pos, bufsiz, p->stime);
    /* 16: cutime  17: cstime */
    pos = fmt_append(buf, pos, bufsiz, " 0 0 ");
    /* 18: priority (default 20) */
    pos = fmt_append(buf, pos, bufsiz, "20 ");
    /* 19: nice */
    pos = fmt_append(buf, pos, bufsiz, "0 ");
    /* 20: num_threads */
    pos = fmt_append(buf, pos, bufsiz, "1 ");
    /* 21: itrealvalue */
    pos = fmt_append(buf, pos, bufsiz, "0 ");
    /* 22: starttime (in jiffies since boot) */
    pos = fmt_append_u32(buf, pos, bufsiz, p->start_time);
    pos = fmt_append(buf, pos, bufsiz, " ");
    /* 23: vsize (bytes) */
    pos = fmt_append_u32(buf, pos, bufsiz, proc_vsz(p));
    pos = fmt_append(buf, pos, bufsiz, " ");
    /* 24: rss (pages) — same as vsz/PAGE_SIZE on PPAP (no swap) */
    pos = fmt_append_u32(buf, pos, bufsiz, proc_vsz(p) / PAGE_SIZE);
    /* Fields 25–52: all zeros */
    pos = fmt_append(buf, pos, bufsiz,
        " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    return pos;
}

/* /proc/<pid>/cmdline — NUL-terminated command name */
static int gen_pid_cmdline(char *buf, int bufsiz, const pcb_t *p)
{
    const char *name = p->comm[0] ? p->comm : "?";
    int len = 0;
    while (name[len] && len < bufsiz - 2)
        buf[len] = name[len], len++;
    buf[len++] = '\0';   /* NUL terminator (part of cmdline format) */
    return len;
}

/* ── Inode numbering scheme ──────────────────────────────────────────────── */
/*
 * Static nodes:  ino = 1 .. PROCFS_NODE_COUNT
 * PID dirs:      ino = 0x1000 + pid * 16
 * PID/stat:      ino = 0x1000 + pid * 16 + 1
 * PID/cmdline:   ino = 0x1000 + pid * 16 + 2
 */
#define PID_INO_BASE    0x1000u
#define PID_INO_STRIDE  16u
#define PID_INO(pid)    (PID_INO_BASE + (uint32_t)(pid) * PID_INO_STRIDE)

/* ── fs_priv encoding for vnodes ─────────────────────────────────────────── */
/*
 * We encode the node type into fs_priv so procfs_read knows what to generate.
 *   - Static nodes:  fs_priv = &procfs_nodes[i]
 *   - PID/stat:      fs_priv = (void*)(0x80000000 | (slot << 8) | 1)
 *   - PID/cmdline:   fs_priv = (void*)(0x80000000 | (slot << 8) | 2)
 */
#define PRIV_PID_FLAG   0x80000000u
#define PRIV_PID_STAT   1u
#define PRIV_PID_CMDLINE 2u
#define MAKE_PID_PRIV(slot, sub) \
    ((void *)(uintptr_t)(PRIV_PID_FLAG | ((uint32_t)(slot) << 8) | (sub)))
#define IS_PID_PRIV(priv)  (((uintptr_t)(priv)) & PRIV_PID_FLAG)
#define PID_PRIV_SLOT(priv) ((((uintptr_t)(priv)) >> 8) & 0xFFu)
#define PID_PRIV_SUB(priv)  (((uintptr_t)(priv)) & 0xFFu)

/* ── procfs_mount ─────────────────────────────────────────────────────────── */

static int procfs_mount(mount_entry_t *mnt, const void *dev_data)
{
    (void)dev_data;

    vnode_t *root = vnode_alloc();
    if (!root)
        return -ENOMEM;

    root->type  = VNODE_DIR;
    root->mode  = S_IFDIR | 0555u;
    root->ino   = 0;
    root->size  = 0;   /* dynamic */
    root->mount = mnt;

    mnt->root = root;
    return 0;
}

/* ── procfs_lookup ────────────────────────────────────────────────────────── */

static int procfs_lookup(vnode_t *dir, const char *name, vnode_t **result)
{
    /* ── Root directory lookup ──────────────────────────────────────── */
    if (dir->ino == 0) {
        /* Check static nodes first */
        for (uint32_t i = 0; i < PROCFS_NODE_COUNT; i++) {
            if (str_eq(procfs_nodes[i].name, name)) {
                vnode_t *vn = vnode_alloc();
                if (!vn) return -ENOMEM;

                vn->type    = VNODE_FILE;
                vn->mode    = S_IFREG | 0444u;
                vn->ino     = i + 1;
                vn->size    = 0;
                vn->mount   = dir->mount;
                vn->fs_priv = (void *)&procfs_nodes[i];
                *result = vn;
                return 0;
            }
        }

        /* Check for numeric PID directories */
        int32_t pid = parse_uint(name);
        if (pid >= 0) {
            for (uint32_t i = 0; i < PROC_MAX; i++) {
                if (proc_table[i].state != PROC_FREE &&
                    proc_table[i].pid == pid) {
                    vnode_t *vn = vnode_alloc();
                    if (!vn) return -ENOMEM;

                    vn->type    = VNODE_DIR;
                    vn->mode    = S_IFDIR | 0555u;
                    vn->ino     = PID_INO((uint32_t)pid);
                    vn->size    = 2;  /* stat + cmdline */
                    vn->mount   = dir->mount;
                    vn->fs_priv = (void *)(uintptr_t)i; /* proc_table slot */
                    *result = vn;
                    return 0;
                }
            }
        }

        return -ENOENT;
    }

    /* ── Per-PID directory lookup ──────────────────────────────────── */
    if (dir->ino >= PID_INO_BASE) {
        uint32_t slot = (uint32_t)(uintptr_t)dir->fs_priv;
        if (slot >= PROC_MAX || proc_table[slot].state == PROC_FREE)
            return -ENOENT;

        uint32_t pid_ino = dir->ino;

        if (str_eq(name, "stat")) {
            vnode_t *vn = vnode_alloc();
            if (!vn) return -ENOMEM;
            vn->type    = VNODE_FILE;
            vn->mode    = S_IFREG | 0444u;
            vn->ino     = pid_ino + 1;
            vn->size    = 0;
            vn->mount   = dir->mount;
            vn->fs_priv = MAKE_PID_PRIV(slot, PRIV_PID_STAT);
            *result = vn;
            return 0;
        }

        if (str_eq(name, "cmdline")) {
            vnode_t *vn = vnode_alloc();
            if (!vn) return -ENOMEM;
            vn->type    = VNODE_FILE;
            vn->mode    = S_IFREG | 0444u;
            vn->ino     = pid_ino + 2;
            vn->size    = 0;
            vn->mount   = dir->mount;
            vn->fs_priv = MAKE_PID_PRIV(slot, PRIV_PID_CMDLINE);
            *result = vn;
            return 0;
        }

        return -ENOENT;
    }

    return -ENOENT;
}

/* ── procfs_read ──────────────────────────────────────────────────────────── */

static long procfs_read(vnode_t *vn, void *buf, size_t n, uint32_t off)
{
    if (vn->type == VNODE_DIR)
        return -(long)EISDIR;

    char tmp[512];
    int total;

    if (IS_PID_PRIV(vn->fs_priv)) {
        /* Per-PID file */
        uint32_t slot = PID_PRIV_SLOT(vn->fs_priv);
        uint32_t sub  = PID_PRIV_SUB(vn->fs_priv);
        if (slot >= PROC_MAX)
            return -(long)EIO;
        const pcb_t *p = &proc_table[slot];
        if (p->state == PROC_FREE)
            return 0;   /* process exited */

        if (sub == PRIV_PID_STAT)
            total = gen_pid_stat(tmp, (int)sizeof(tmp), p);
        else if (sub == PRIV_PID_CMDLINE)
            total = gen_pid_cmdline(tmp, (int)sizeof(tmp), p);
        else
            return -(long)EIO;
    } else {
        /* Static node */
        const procfs_node_t *node = (const procfs_node_t *)vn->fs_priv;
        if (!node || !node->generate)
            return -(long)EIO;
        total = node->generate(tmp, (int)sizeof(tmp));
    }

    if (total < 0)
        return -(long)EIO;
    if (off >= (uint32_t)total)
        return 0;

    uint32_t avail = (uint32_t)total - off;
    if (n > avail)
        n = avail;

    __builtin_memcpy(buf, tmp + off, n);
    return (long)n;
}

/* ── procfs_readdir ───────────────────────────────────────────────────────── */

static int procfs_readdir(vnode_t *dir, struct dirent *entries,
                           size_t max_entries, uint32_t *cookie)
{
    /* ── Root directory ────────────────────────────────────────────── */
    if (dir->ino == 0) {
        uint32_t idx = *cookie;
        int count = 0;

        /* Phase 1: static nodes */
        while (idx < PROCFS_NODE_COUNT && (size_t)count < max_entries) {
            const procfs_node_t *node = &procfs_nodes[idx];
            entries[count].d_ino  = idx + 1;
            entries[count].d_type = DT_REG;
            uint32_t nlen = str_len(node->name);
            if (nlen > VFS_NAME_MAX) nlen = VFS_NAME_MAX;
            __builtin_memcpy(entries[count].d_name, node->name, nlen);
            entries[count].d_name[nlen] = '\0';
            idx++;
            count++;
        }

        /* Phase 2: PID directories (index offset by PROCFS_NODE_COUNT) */
        uint32_t pid_idx = (idx >= PROCFS_NODE_COUNT)
                           ? idx - PROCFS_NODE_COUNT : 0;
        while (pid_idx < PROC_MAX && (size_t)count < max_entries) {
            if (proc_table[pid_idx].state != PROC_FREE) {
                entries[count].d_ino  = PID_INO((uint32_t)proc_table[pid_idx].pid);
                entries[count].d_type = DT_DIR;
                /* Convert PID to string */
                char pid_str[12];
                fmt_u32(pid_str, (uint32_t)proc_table[pid_idx].pid);
                uint32_t plen = str_len(pid_str);
                __builtin_memcpy(entries[count].d_name, pid_str, plen);
                entries[count].d_name[plen] = '\0';
                count++;
            }
            pid_idx++;
            idx = PROCFS_NODE_COUNT + pid_idx;
        }

        *cookie = idx;
        return count;
    }

    /* ── Per-PID directory ─────────────────────────────────────────── */
    if (dir->ino >= PID_INO_BASE) {
        uint32_t idx = *cookie;
        int count = 0;
        uint32_t pid_ino = dir->ino;

        static const struct { const char *name; uint32_t off; } pid_entries[] = {
            { "stat",    1 },
            { "cmdline", 2 },
        };
        uint32_t nentries = sizeof(pid_entries) / sizeof(pid_entries[0]);

        while (idx < nentries && (size_t)count < max_entries) {
            entries[count].d_ino  = pid_ino + pid_entries[idx].off;
            entries[count].d_type = DT_REG;
            uint32_t nlen = str_len(pid_entries[idx].name);
            __builtin_memcpy(entries[count].d_name, pid_entries[idx].name, nlen);
            entries[count].d_name[nlen] = '\0';
            idx++;
            count++;
        }

        *cookie = idx;
        return count;
    }

    return 0;
}

/* ── procfs_stat ──────────────────────────────────────────────────────────── */

static int procfs_stat(vnode_t *vn, struct stat *st)
{
    st->st_ino   = vn->ino;
    st->st_mode  = vn->mode;
    st->st_nlink = 1;
    st->st_size  = vn->size;
    return 0;
}

/* ── procfs_statfs ─────────────────────────────────────────────────────────── */

static int procfs_statfs(mount_entry_t *mnt, struct kernel_statfs *buf)
{
    (void)mnt;
    __builtin_memset(buf, 0, sizeof(*buf));

    buf->f_type    = 0x9FA0u;           /* Linux PROC_SUPER_MAGIC */
    buf->f_bsize   = PAGE_SIZE;
    buf->f_frsize  = PAGE_SIZE;
    buf->f_namelen = VFS_NAME_MAX;
    buf->f_flags   = 1;                /* ST_RDONLY */
    return 0;
}

/* ── Operations table ─────────────────────────────────────────────────────── */

const vfs_ops_t procfs_ops = {
    .mount    = procfs_mount,
    .lookup   = procfs_lookup,
    .read     = procfs_read,
    .write    = NULL,    /* read-only filesystem */
    .readdir  = procfs_readdir,
    .stat     = procfs_stat,
    .readlink = NULL,    /* no symlinks in procfs */
    .statfs   = procfs_statfs,
};
