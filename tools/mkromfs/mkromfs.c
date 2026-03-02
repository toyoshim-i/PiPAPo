/*
 * mkromfs.c — Host tool: build a PPAP romfs image from a directory tree
 *
 * Usage:
 *   mkromfs <input_dir> <output_file>   — build romfs image
 *   mkromfs --dump <romfs_file>         — dump image contents (debug)
 *
 * The output is a binary romfs image with a romfs_super_t header followed
 * by romfs_entry_t entries.  The image is position-independent (all
 * offsets are relative to the image start).
 *
 * Algorithm:
 *   Pass 1 — Scan the directory tree, build an in-memory node list,
 *            compute entry sizes and assign offsets.
 *   Pass 2 — Write entries to the output buffer with correct offsets.
 *
 * Compiled with the host gcc (not cross-compiled):
 *   gcc -o mkromfs mkromfs.c -I../../src/kernel/fs
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "romfs_format.h"

/* ── Limits ────────────────────────────────────────────────────────────────── */

#define MAX_NODES   512        /* max files+dirs+symlinks in the tree */
#define MAX_IMAGE   (2 << 20) /* 2 MB max image size */
#define MAX_PATH    1024

/* ── In-memory node representation ─────────────────────────────────────────── */

typedef struct node {
    char     name[256];       /* entry name (filename only, not full path) */
    char     path[MAX_PATH];  /* full host path for reading file data      */
    uint32_t type;            /* ROMFS_TYPE_FILE / DIR / SYMLINK           */
    uint32_t size;            /* file: content size; symlink: target len   */
    char     link_target[256];/* symlink target (if type == SYMLINK)       */

    /* Tree structure (indices into node array, -1 = none) */
    int      first_child;
    int      next_sibling;

    /* Assigned during layout */
    uint32_t offset;          /* offset in output image                    */
    uint32_t entry_size;      /* total on-flash size (header+name+data)    */
} node_t;

static node_t nodes[MAX_NODES];
static int    node_count = 0;

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static uint32_t align4(uint32_t x)
{
    return (x + 3u) & ~3u;
}

static uint32_t entry_size(const node_t *n)
{
    uint32_t name_padded = align4((uint32_t)strlen(n->name) + 1);
    uint32_t data_padded = align4(n->size);
    return (uint32_t)sizeof(romfs_entry_t) + name_padded + data_padded;
}

/* ── Pass 1: scan directory tree recursively ───────────────────────────────── */

static int scan_dir(const char *host_path, int parent_idx);

static int add_node(const char *host_path, const char *name,
                    uint32_t type, int parent_idx)
{
    if (node_count >= MAX_NODES) {
        fprintf(stderr, "mkromfs: too many nodes (max %d)\n", MAX_NODES);
        return -1;
    }

    int idx = node_count++;
    node_t *n = &nodes[idx];

    strncpy(n->name, name, sizeof(n->name) - 1);
    n->name[sizeof(n->name) - 1] = '\0';
    strncpy(n->path, host_path, sizeof(n->path) - 1);
    n->path[sizeof(n->path) - 1] = '\0';
    n->type = type;
    n->size = 0;
    n->link_target[0] = '\0';
    n->first_child = -1;
    n->next_sibling = -1;
    n->offset = 0;
    n->entry_size = 0;

    /* Link into parent's child list */
    if (parent_idx >= 0) {
        node_t *parent = &nodes[parent_idx];
        if (parent->first_child < 0) {
            parent->first_child = idx;
        } else {
            /* Append to end of sibling chain */
            int sib = parent->first_child;
            while (nodes[sib].next_sibling >= 0)
                sib = nodes[sib].next_sibling;
            nodes[sib].next_sibling = idx;
        }
    }

    if (type == ROMFS_TYPE_FILE) {
        struct stat st;
        if (stat(host_path, &st) == 0)
            n->size = (uint32_t)st.st_size;
    } else if (type == ROMFS_TYPE_SYMLINK) {
        ssize_t len = readlink(host_path, n->link_target,
                               sizeof(n->link_target) - 1);
        if (len < 0) {
            perror("readlink");
            return -1;
        }
        n->link_target[len] = '\0';
        n->size = (uint32_t)len;
    } else if (type == ROMFS_TYPE_DIR) {
        if (scan_dir(host_path, idx) < 0)
            return -1;
    }

    n->entry_size = entry_size(n);
    return idx;
}

static int scan_dir(const char *host_path, int parent_idx)
{
    DIR *d = opendir(host_path);
    if (!d) {
        perror(host_path);
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char child_path[MAX_PATH];
        snprintf(child_path, sizeof(child_path), "%s/%s",
                 host_path, ent->d_name);

        struct stat st;
        if (lstat(child_path, &st) < 0) {
            perror(child_path);
            closedir(d);
            return -1;
        }

        uint32_t type;
        if (S_ISDIR(st.st_mode))
            type = ROMFS_TYPE_DIR;
        else if (S_ISLNK(st.st_mode))
            type = ROMFS_TYPE_SYMLINK;
        else if (S_ISREG(st.st_mode))
            type = ROMFS_TYPE_FILE;
        else
            continue;   /* skip special files */

        if (add_node(child_path, ent->d_name, type, parent_idx) < 0) {
            closedir(d);
            return -1;
        }
    }

    closedir(d);
    return 0;
}

/* ── Pass 2: assign offsets ────────────────────────────────────────────────── */

static void assign_offsets(void)
{
    /* Offset 0: superblock */
    uint32_t off = (uint32_t)sizeof(romfs_super_t);

    for (int i = 0; i < node_count; i++) {
        nodes[i].offset = off;
        off += nodes[i].entry_size;
    }
}

/* ── Pass 3: write image ───────────────────────────────────────────────────── */

static int write_image(uint8_t *buf, uint32_t buf_size, uint32_t *out_size)
{
    assign_offsets();

    uint32_t total = (uint32_t)sizeof(romfs_super_t);
    for (int i = 0; i < node_count; i++)
        total += nodes[i].entry_size;

    if (total > buf_size) {
        fprintf(stderr, "mkromfs: image too large (%u bytes, max %u)\n",
                total, buf_size);
        return -1;
    }

    memset(buf, 0, total);

    /* Write superblock */
    romfs_super_t *sb = (romfs_super_t *)buf;
    sb->magic = ROMFS_MAGIC;
    sb->size = total;
    sb->file_count = (uint32_t)node_count;
    sb->root_off = (node_count > 0) ? nodes[0].offset : 0;

    /* Write each entry */
    for (int i = 0; i < node_count; i++) {
        node_t *n = &nodes[i];
        romfs_entry_t *e = (romfs_entry_t *)(buf + n->offset);

        e->next_off = (n->next_sibling >= 0)
                      ? nodes[n->next_sibling].offset : 0;
        e->type = n->type;
        e->size = n->size;
        e->child_off = (n->first_child >= 0)
                       ? nodes[n->first_child].offset : 0;
        e->name_len = (uint32_t)strlen(n->name);

        /* Copy name (NUL-terminated) */
        char *name_dst = (char *)(buf + n->offset + ROMFS_NAME_OFF);
        memcpy(name_dst, n->name, e->name_len + 1);

        /* Copy data (file content or symlink target) */
        if (n->type == ROMFS_TYPE_FILE && n->size > 0) {
            uint8_t *data_dst = buf + n->offset + ROMFS_DATA_OFF(e);
            FILE *f = fopen(n->path, "rb");
            if (!f) {
                perror(n->path);
                return -1;
            }
            size_t rd = fread(data_dst, 1, n->size, f);
            fclose(f);
            if (rd != n->size) {
                fprintf(stderr, "mkromfs: short read on %s\n", n->path);
                return -1;
            }
        } else if (n->type == ROMFS_TYPE_SYMLINK && n->size > 0) {
            char *data_dst = (char *)(buf + n->offset + ROMFS_DATA_OFF(e));
            memcpy(data_dst, n->link_target, n->size);
        }
    }

    *out_size = total;
    return 0;
}

/* ── Dump mode ─────────────────────────────────────────────────────────────── */

static void dump_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = malloc((size_t)fsize);
    if (!buf) { perror("malloc"); exit(1); }
    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        fprintf(stderr, "short read\n"); exit(1);
    }
    fclose(f);

    const romfs_super_t *sb = (const romfs_super_t *)buf;
    if (sb->magic != ROMFS_MAGIC) {
        fprintf(stderr, "bad magic: 0x%08X (expected 0x%08X)\n",
                sb->magic, ROMFS_MAGIC);
        free(buf);
        exit(1);
    }

    printf("romfs image: %u bytes, %u entries\n", sb->size, sb->file_count);
    printf("  super { magic=0x%08X size=%u files=%u root=0x%04X }\n",
           sb->magic, sb->size, sb->file_count, sb->root_off);

    /* Walk all entries sequentially */
    uint32_t off = sb->root_off;
    for (uint32_t i = 0; i < sb->file_count; i++) {
        if (off + sizeof(romfs_entry_t) > (uint32_t)fsize) {
            printf("  [truncated at offset 0x%04X]\n", off);
            break;
        }
        const romfs_entry_t *e = (const romfs_entry_t *)(buf + off);
        const char *name = (const char *)(buf + off + ROMFS_NAME_OFF);
        const char *type_str = (e->type == ROMFS_TYPE_DIR)     ? "DIR" :
                               (e->type == ROMFS_TYPE_FILE)    ? "FILE" :
                               (e->type == ROMFS_TYPE_SYMLINK) ? "LINK" :
                                                                  "???";

        printf("  0x%04X: %-4s %-20s  size=%-5u child=0x%04X next=0x%04X",
               off, type_str, name, e->size, e->child_off, e->next_off);

        if (e->type == ROMFS_TYPE_SYMLINK && e->size > 0) {
            const char *target = (const char *)(buf + off + ROMFS_DATA_OFF(e));
            printf("  -> %.*s", (int)e->size, target);
        }
        printf("\n");

        off += ROMFS_ENTRY_SIZE(e);
    }

    free(buf);
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc == 3 && strcmp(argv[1], "--dump") == 0) {
        dump_image(argv[2]);
        return 0;
    }

    if (argc != 3) {
        fprintf(stderr, "Usage: mkromfs <input_dir> <output_file>\n");
        fprintf(stderr, "       mkromfs --dump <romfs_file>\n");
        return 1;
    }

    const char *input_dir = argv[1];
    const char *output_file = argv[2];

    /* Verify input is a directory */
    struct stat st;
    if (stat(input_dir, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "mkromfs: %s is not a directory\n", input_dir);
        return 1;
    }

    /* Pass 1: scan tree — root node is the directory itself */
    int root_idx = add_node(input_dir, "/", ROMFS_TYPE_DIR, -1);
    if (root_idx < 0)
        return 1;

    printf("mkromfs: scanned %d entries from %s\n", node_count, input_dir);

    /* Pass 2+3: layout and write */
    uint8_t *buf = calloc(1, MAX_IMAGE);
    if (!buf) { perror("calloc"); return 1; }

    uint32_t image_size = 0;
    if (write_image(buf, MAX_IMAGE, &image_size) < 0) {
        free(buf);
        return 1;
    }

    /* Write to file */
    FILE *f = fopen(output_file, "wb");
    if (!f) { perror(output_file); free(buf); return 1; }
    if (fwrite(buf, 1, image_size, f) != image_size) {
        perror("fwrite");
        fclose(f);
        free(buf);
        return 1;
    }
    fclose(f);
    free(buf);

    printf("mkromfs: wrote %u bytes to %s\n", image_size, output_file);
    return 0;
}
