/*
 * test_ufs.c — UFS write+read tests on writable UFS filesystem
 *
 * Probes for the UFS mount point: /mnt/ufs (ARM/m68k) or / (pcxt).
 * Tests create, write, read, append, and unlink on UFS.
 */

#include "utest.h"

/* Probe for writable UFS: try /mnt/ufs first, then / */
static const char *ufs_base;

static int probe_ufs(void)
{
    struct stat st;
    if (stat("/mnt/ufs", &st) == 0) { ufs_base = "/mnt/ufs/"; return 1; }
    /* On pcxt, root is UFS — try creating a file at / */
    int fd = open("/.ufs_probe", O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) {
        close(fd);
        unlink("/.ufs_probe");
        ufs_base = "/";
        return 1;
    }
    return 0;
}

/* Build path: ufs_base + name into buf */
static void make_path(char *buf, int bufsz, const char *name)
{
    int i = 0, j = 0;
    while (ufs_base[j] && i < bufsz - 1) buf[i++] = ufs_base[j++];
    j = 0;
    while (name[j] && i < bufsz - 1) buf[i++] = name[j++];
    buf[i] = 0;
}

int main(void)
{
    int i;
    char path[64];

    if (!probe_ufs()) {
        UT_PRINT("test_ufs: no writable UFS found, skipping\n");
        UT_SUMMARY("test_ufs");
    }

    /* 1. Create and write a small file */
    make_path(path, (int)sizeof(path), "test_ufs_file");
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    UT_ASSERT(fd >= 0, "create test_ufs_file");
    if (fd >= 0) {
        ssize_t nw = write(fd, "HELLO", 5);
        UT_ASSERT_EQ(nw, 5);
        close(fd);
    }

    /* 2. Read back and verify */
    fd = open(path, O_RDONLY, 0);
    UT_ASSERT(fd >= 0, "open for read");
    if (fd >= 0) {
        char buf[8];
        for (i = 0; i < 8; i++) buf[i] = 0;
        ssize_t nr = read(fd, buf, 8);
        UT_ASSERT_EQ(nr, 5);
        UT_ASSERT(buf[0] == 'H' && buf[1] == 'E' && buf[2] == 'L' &&
                  buf[3] == 'L' && buf[4] == 'O', "content matches");
        close(fd);
    }

    /* 3. Unlink */
    int ret = unlink(path);
    UT_ASSERT_EQ(ret, 0);

    /* 4. Stat after unlink should fail */
    {
        struct stat st;
        ret = stat(path, &st);
        UT_ASSERT(ret < 0, "stat after unlink fails");
    }

    /* 5. Larger write (512 bytes, one sector) + read back */
    make_path(path, (int)sizeof(path), "test_ufs_large");
    {
        char wbuf[512];
        for (i = 0; i < (int)sizeof(wbuf); i++)
            wbuf[i] = (char)(0x30 + (i % 10));

        fd = open(path, O_WRONLY | O_CREAT, 0644);
        UT_ASSERT(fd >= 0, "create large file");
        if (fd >= 0) {
            ssize_t nw = write(fd, wbuf, sizeof(wbuf));
            UT_ASSERT_EQ(nw, (int)sizeof(wbuf));
            close(fd);
        }

        char rbuf[512];
        for (i = 0; i < (int)sizeof(rbuf); i++) rbuf[i] = 0;
        fd = open(path, O_RDONLY, 0);
        UT_ASSERT(fd >= 0, "open large for read");
        if (fd >= 0) {
            ssize_t nr = read(fd, rbuf, sizeof(rbuf));
            UT_ASSERT_EQ(nr, (int)sizeof(rbuf));

            int mismatch = 0;
            for (i = 0; i < (int)sizeof(rbuf); i++) {
                if (rbuf[i] != (char)(0x30 + (i % 10))) {
                    mismatch = i + 1;
                    break;
                }
            }
            UT_ASSERT_EQ(mismatch, 0);
            close(fd);
        }

        unlink(path);
    }

    /* 6. Append mode */
    make_path(path, (int)sizeof(path), "test_ufs_append");
    {
        fd = open(path, O_WRONLY | O_CREAT, 0644);
        UT_ASSERT(fd >= 0, "create append file");
        if (fd >= 0) {
            write(fd, "AB", 2);
            close(fd);
        }

        fd = open(path, O_WRONLY | O_APPEND, 0);
        UT_ASSERT(fd >= 0, "open for append");
        if (fd >= 0) {
            write(fd, "CD", 2);
            close(fd);
        }

        char buf[8];
        for (i = 0; i < 8; i++) buf[i] = 0;
        fd = open(path, O_RDONLY, 0);
        UT_ASSERT(fd >= 0, "read appended file");
        if (fd >= 0) {
            ssize_t nr = read(fd, buf, 8);
            UT_ASSERT_EQ(nr, 4);
            UT_ASSERT(buf[0] == 'A' && buf[1] == 'B' &&
                      buf[2] == 'C' && buf[3] == 'D', "append content ABCD");
            close(fd);
        }

        unlink(path);
    }

    UT_SUMMARY("test_ufs");
}
