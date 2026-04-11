/*
 * test_tmpfs.c — mkdir, unlink, rmdir on /tmp (tmpfs)
 */

#include "utest.h"

/* Helper: compare two strings */
static int streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

int main(void)
{
    /* 1. /tmp should exist and be a directory */
    struct stat st;
    int ret = stat("/tmp", &st);
    UT_ASSERT_EQ(ret, 0);
    UT_ASSERT(S_ISDIR(st.st_mode), "/tmp is directory");

    /* 2. Create a file in /tmp: open + write + close */
    int fd = open("/tmp/test_tmpfs_file", O_WRONLY | O_CREAT, 0644);
    UT_ASSERT(fd >= 0, "create /tmp/test_tmpfs_file");
    if (fd >= 0) {
        write(fd, "HELLO", 5);
        close(fd);
    }

    /* 3. stat the created file */
    ret = stat("/tmp/test_tmpfs_file", &st);
    UT_ASSERT_EQ(ret, 0);
    UT_ASSERT(S_ISREG(st.st_mode), "tmpfs file is regular");
    UT_ASSERT_EQ((int)st.st_size, 5);

    /* 4. Read back the file */
    fd = open("/tmp/test_tmpfs_file", O_RDONLY, 0);
    UT_ASSERT(fd >= 0, "open tmpfs file for read");
    if (fd >= 0) {
        char buf[8];
        int i;
        for (i = 0; i < 8; i++) buf[i] = 0;
        ssize_t n = read(fd, buf, 8);
        UT_ASSERT_EQ(n, 5);
        UT_ASSERT(streq(buf, "HELLO"), "tmpfs read matches write");
        close(fd);
    }

    /* 5. unlink the file */
    ret = unlink("/tmp/test_tmpfs_file");
    UT_ASSERT_EQ(ret, 0);

    /* 6. stat after unlink should fail */
    ret = stat("/tmp/test_tmpfs_file", &st);
    UT_ASSERT(ret < 0, "stat after unlink fails");

    /* 7. mkdir in /tmp */
    ret = mkdir("/tmp/test_subdir", 0755);
    UT_ASSERT_EQ(ret, 0);

    /* 8. stat the new directory */
    ret = stat("/tmp/test_subdir", &st);
    UT_ASSERT_EQ(ret, 0);
    UT_ASSERT(S_ISDIR(st.st_mode), "mkdir created directory");

    /* 9. rmdir */
    ret = rmdir("/tmp/test_subdir");
    UT_ASSERT_EQ(ret, 0);

    /* 10. stat after rmdir should fail */
    ret = stat("/tmp/test_subdir", &st);
    UT_ASSERT(ret < 0, "stat after rmdir fails");

    /* 11. unlink nonexistent should fail */
    ret = unlink("/tmp/no_such_file");
    UT_ASSERT(ret < 0, "unlink nonexistent fails");

    /* 12. rmdir nonexistent should fail */
    ret = rmdir("/tmp/no_such_dir");
    UT_ASSERT(ret < 0, "rmdir nonexistent fails");

    /* 13. Large write + read (4096 bytes, full tmpfs file capacity).
     * The user buffer may span a page boundary depending on stack
     * alignment, exercising the vfs_bridge page-walk loop. */
    {
        char wbuf[4096];
        int i;
        for (i = 0; i < (int)sizeof(wbuf); i++)
            wbuf[i] = (char)(i & 0x7f);

        fd = open("/tmp/test_large", O_WRONLY | O_CREAT, 0644);
        UT_ASSERT(fd >= 0, "create large file");
        if (fd >= 0) {
            ssize_t nw = write(fd, wbuf, sizeof(wbuf));
            UT_ASSERT_EQ(nw, (int)sizeof(wbuf));
            close(fd);
        }

        /* Read back in one call */
        char rbuf[4096];
        for (i = 0; i < (int)sizeof(rbuf); i++)
            rbuf[i] = 0;

        fd = open("/tmp/test_large", O_RDONLY, 0);
        UT_ASSERT(fd >= 0, "open large file for read");
        if (fd >= 0) {
            ssize_t nr = read(fd, rbuf, sizeof(rbuf));
            UT_ASSERT_EQ(nr, (int)sizeof(rbuf));

            /* Verify content */
            int mismatch = 0;
            for (i = 0; i < (int)sizeof(rbuf); i++) {
                if (rbuf[i] != (char)(i & 0x7f)) { mismatch = i + 1; break; }
            }
            UT_ASSERT_EQ(mismatch, 0);
            close(fd);
        }

        unlink("/tmp/test_large");
    }

    /* 14. Read straddling a page boundary in the user buffer.
     * Allocate two adjacent buffers so the second starts near the
     * end of a stack frame, making it likely that part of buf[]
     * falls in one page and part in the next. We force this by
     * using a gap variable to push buf to a known offset. */
    {
        char gap[3900]; /* push buf near page boundary */
        char buf[256];
        int i;
        gap[0] = 0; /* prevent optimizer from removing gap */
        (void)gap;

        /* Write a pattern */
        for (i = 0; i < (int)sizeof(buf); i++)
            buf[i] = (char)(0x41 + (i % 26));

        fd = open("/tmp/test_straddle", O_WRONLY | O_CREAT, 0644);
        UT_ASSERT(fd >= 0, "create straddle file");
        if (fd >= 0) {
            ssize_t nw = write(fd, buf, sizeof(buf));
            UT_ASSERT_EQ(nw, (int)sizeof(buf));
            close(fd);
        }

        /* Clear and read back */
        for (i = 0; i < (int)sizeof(buf); i++)
            buf[i] = 0;

        fd = open("/tmp/test_straddle", O_RDONLY, 0);
        UT_ASSERT(fd >= 0, "open straddle file for read");
        if (fd >= 0) {
            ssize_t nr = read(fd, buf, sizeof(buf));
            UT_ASSERT_EQ(nr, (int)sizeof(buf));

            int mismatch = 0;
            for (i = 0; i < (int)sizeof(buf); i++) {
                if (buf[i] != (char)(0x41 + (i % 26))) { mismatch = i + 1; break; }
            }
            UT_ASSERT_EQ(mismatch, 0);
            close(fd);
        }

        unlink("/tmp/test_straddle");
    }

    UT_SUMMARY("test_tmpfs");
}
